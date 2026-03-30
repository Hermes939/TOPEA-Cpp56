// ============================================================
//  TOP4EA — grid_engine.cpp
//  Calcul adaptatif des niveaux SS/BS avec ATR
// ============================================================
#include "../include/grid_engine.hpp"
#include <cmath>
#include <algorithm>

namespace top4ea {

GridEngine::GridEngine(const Config& cfg)
    : cfg_(cfg)
    , atr_(cfg.atrPeriod)
    , volReg_(200)
    , spreadTrack_(cfg.spreadAvgAlpha)
    , ouEst_(cfg.ouWindowSize, cfg.ouMinTheta)
{}

void GridEngine::onTick(const Tick& tick) noexcept {
    std::lock_guard lk(mu_);
    lastBid_ = tick.bid;
    lastAsk_ = tick.ask;
    spreadTrack_.push(tick.spread);

    // [A] Alimentation estimateur OU avec le prix mid
    double mid = (tick.bid + tick.ask) * 0.5;
    ouEst_.push(mid);

    // Mise à jour High/Low intra-tick pour ATR synthetic
    if (lastHigh_ == 0.0) lastHigh_ = tick.ask;
    if (lastLow_  == 0.0) lastLow_  = tick.bid;
    lastHigh_ = std::max(lastHigh_, tick.ask);
    lastLow_  = std::min(lastLow_,  tick.bid);
}

void GridEngine::pushOHLC(double o, double h, double l, double c) noexcept {
    std::lock_guard lk(mu_);
    double prev = (prevClose_ == 0.0) ? o : prevClose_;
    double atrV = atr_.update(h, l, prev);
    prevClose_  = c;
    lastHigh_   = h;
    lastLow_    = l;
    volReg_.push(atrV);
    regime_ = volReg_.classify(atrV);
}

double GridEngine::pointSize() const noexcept {
    return (cfg_.digits == 5 || cfg_.digits == 3)
           ? cfg_.point : cfg_.point;
}

double GridEngine::atrPoints() const noexcept {
    return atr_.ready() ? atr_.toPoints(pointSize()) : (double)cfg_.gridGap;
}

int GridEngine::ouGridGap() const noexcept {
    if (!cfg_.useOUGrid || !ouEst_.ready()) return 0;
    double gapPrice = ouEst_.optimalGapPrice();
    if (gapPrice <= 0.0) return 0;
    int pts = (int)std::round(gapPrice / pointSize());
    return std::clamp(pts, (int)cfg_.atrGridMin, (int)cfg_.atrGridMax);
}

int GridEngine::effectiveGridGap() const noexcept {
    // Priorité 1 : OU optimal si marché mean-reverting et activé
    if (cfg_.useOUGrid) {
        int ou = ouGridGap();
        if (ou > 0) return ou;
        // Fallback si trending : ATR ou fixe
    }
    // Priorité 2 : ATR dynamique
    if (cfg_.useAtrGrid && atr_.ready())
        return math::dynamicGridGap(atrPoints(),
                                    cfg_.atrMultiplier,
                                    cfg_.atrGridMin,
                                    cfg_.atrGridMax);
    // Priorité 3 : GridGap fixe de config
    return cfg_.gridGap;
}

double GridEngine::halfGap() const noexcept {
    return (effectiveGridGap() / 2.0) * pointSize();
}

double GridEngine::minDistPts() const noexcept {
    // équivalent MODE_STOPLEVEL * Point + 2*Point
    // valeur typique XAU : 100 points
    return 100.0 * pointSize() + 2.0 * pointSize();
}

bool GridEngine::isPriceValid(double price, double minDist,
                              OrderType stopType) const noexcept {
    if (stopType == OrderType::SellStop)
        return (lastBid_ - price) >= minDist;
    if (stopType == OrderType::BuyStop)
        return (price - lastAsk_) >= minDist;
    return true;
}

StopPlacement GridEngine::computeSellStop(
    int    currentTicket,
    double currentLevel,
    double anchorPrice,
    int    recovDepth,
    double gapFactor) const noexcept
{
    std::lock_guard lk(mu_);
    StopPlacement sp;

    // ── Ancrage géométrique pur ────────────────────────────────
    // Le SellStop est placé à EXACTEMENT (depth+1) niveaux de grille
    // EN DESSOUS de l'origine. Jamais calculé depuis le prix courant.
    // Si anchorPrice non encore établi → fallback bid courant (init)
    double anchor = (anchorPrice > 0.0) ? anchorPrice : lastBid_;

    // Pas géométrique : 1 GridGap complet × facteur asymétrique intelligence
    double step = effectiveGridGap() * gapFactor * pointSize();

    // Niveau fixe : origin - (depth+1) × step
    double target = math::normalize(anchor - (recovDepth + 1) * step, cfg_.digits);

    // Garde broker : le stop doit respecter le STOPLEVEL minimum
    double mdist = minDistPts();
    if ((lastBid_ - target) < mdist)
        target = math::normalize(lastBid_ - mdist, cfg_.digits);

    sp.targetPrice  = target;
    sp.currentPrice = currentLevel;

    if (currentTicket <= 0) {
        // Stop absent → création au niveau géométrique
        sp.needsCreate = true;
    } else {
        sp.delta = target - currentLevel;
        // Modification uniquement si le niveau géométrique a changé
        // (gap ATR recalculé ou facteur intelligence modifié)
        // Seuil = 1 point pour éviter oscillations numériques
        double minChange = pointSize();
        sp.needsModify = (std::abs(sp.delta) > minChange);
    }
    return sp;
}

StopPlacement GridEngine::computeBuyStop(
    int    currentTicket,
    double currentLevel,
    double anchorPrice,
    int    recovDepth,
    double gapFactor) const noexcept
{
    std::lock_guard lk(mu_);
    StopPlacement sp;

    // ── Ancrage géométrique pur ────────────────────────────────
    // Le BuyStop est placé à EXACTEMENT (depth+1) niveaux de grille
    // AU DESSUS de l'origine. Jamais calculé depuis le prix courant.
    double anchor = (anchorPrice > 0.0) ? anchorPrice : lastAsk_;

    // Pas géométrique : 1 GridGap complet × facteur asymétrique intelligence
    double step = effectiveGridGap() * gapFactor * pointSize();

    // Niveau fixe : origin + (depth+1) × step
    double target = math::normalize(anchor + (recovDepth + 1) * step, cfg_.digits);

    // Garde broker : le stop doit respecter le STOPLEVEL minimum
    double mdist = minDistPts();
    if ((target - lastAsk_) < mdist)
        target = math::normalize(lastAsk_ + mdist, cfg_.digits);

    sp.targetPrice  = target;
    sp.currentPrice = currentLevel;

    if (currentTicket <= 0) {
        sp.needsCreate = true;
    } else {
        sp.delta = currentLevel - target;
        double minChange = pointSize();
        sp.needsModify = (std::abs(sp.delta) > minChange);
    }
    return sp;
}

} // namespace top4ea
