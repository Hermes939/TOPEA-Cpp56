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
    lastBid_.store(tick.bid, std::memory_order_relaxed);
    lastAsk_.store(tick.ask, std::memory_order_relaxed);
    std::lock_guard lk(mu_);
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
    regime_.store(volReg_.classify(atrV), std::memory_order_relaxed);
}

double GridEngine::pointSize() const noexcept {
    return (cfg_.digits == 5 || cfg_.digits == 3)
           ? cfg_.point : cfg_.point;
}

double GridEngine::atrPoints() const noexcept {
    std::lock_guard lk(mu_);
    return atr_.ready() ? atr_.toPoints(pointSize()) : (double)cfg_.gridGap;
}

int GridEngine::ouGridGap() const noexcept {
    std::lock_guard lk(mu_);
    if (!cfg_.useOUGrid || !ouEst_.ready()) return 0;
    double gapPrice = ouEst_.optimalGapPrice();
    if (gapPrice <= 0.0) return 0;
    int pts = (int)std::round(gapPrice / pointSize());
    return std::clamp(pts, (int)cfg_.atrGridMin, (int)cfg_.atrGridMax);
}

int GridEngine::effectiveGridGap() const noexcept {
    std::lock_guard lk(mu_);
    // Priorité 1 : OU optimal si marché mean-reverting et activé
    if (cfg_.useOUGrid) {
        // Re-entrant: mu_ est recursive_mutex, l'appel interne est sûr
        int ou = ouGridGap();
        if (ou > 0) return ou;
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
    double bid = lastBid_.load(std::memory_order_relaxed);
    double ask = lastAsk_.load(std::memory_order_relaxed);
    if (stopType == OrderType::SellStop)
        return (bid - price) >= minDist;
    if (stopType == OrderType::BuyStop)
        return (price - ask) >= minDist;
    return true;
}

StopPlacement GridEngine::computeSellStop(
    int    currentTicket,
    double currentLevel,
    double /*anchorPrice*/,
    int    /*recovDepth*/,
    double gapFactor) const noexcept
{
    std::lock_guard lk(mu_);
    StopPlacement sp;

    double bid = lastBid_.load(std::memory_order_relaxed);

    // ── Ancrage flottant : toujours 1 demi-gap sous le bid courant ─
    // Le GridGap est partagé symétriquement : halfGap côté SS + halfGap
    // côté BS → 500 pts total entre les deux stops pour 2 décimales,
    // 5000 pts pour 3 décimales (50 pts × 2 = 100 pts, etc.).
    // La profondeur de récupération pilote le lot (Kelly), pas la position.
    double step = halfGap() * gapFactor;   // demi-gap × facteur intel
    double target = math::normalize(bid - step, cfg_.digits);

    // Garde broker : respect du STOPLEVEL minimum
    double mdist = minDistPts();
    if ((bid - target) < mdist)
        target = math::normalize(bid - mdist, cfg_.digits);

    sp.targetPrice  = target;
    sp.currentPrice = currentLevel;

    if (currentTicket <= 0) {
        sp.needsCreate = true;
    } else {
        sp.delta = target - currentLevel;
        // ── RATCHET HAUT UNIQUEMENT + trail réactif ────────────
        // Seuil = 10% du demi-gap (~25 pts pour gridGap=500).
        // Le SS monte dès que le bid monte d'au moins 25 pts,
        // maintenant en permanence une distance de 250 pts.
        double minChange = halfGap() / 10.0;
        sp.needsModify = (sp.delta > minChange);
    }
    return sp;
}

StopPlacement GridEngine::computeBuyStop(
    int    currentTicket,
    double currentLevel,
    double /*anchorPrice*/,
    int    /*recovDepth*/,
    double gapFactor) const noexcept
{
    std::lock_guard lk(mu_);
    StopPlacement sp;

    double ask = lastAsk_.load(std::memory_order_relaxed);

    // ── Ancrage flottant : toujours 1 demi-gap au-dessus du ask courant ─
    // Symétrique au SS : halfGap côté BS + halfGap côté SS = 1 GridGap
    // total entre les deux stops.
    double step = halfGap() * gapFactor;   // demi-gap × facteur intel
    double target = math::normalize(ask + step, cfg_.digits);

    // Garde broker : respect du STOPLEVEL minimum
    double mdist = minDistPts();
    if ((target - ask) < mdist)
        target = math::normalize(ask + mdist, cfg_.digits);

    sp.targetPrice  = target;
    sp.currentPrice = currentLevel;

    if (currentTicket <= 0) {
        sp.needsCreate = true;
    } else {
        sp.delta = currentLevel - target;
        // ── RATCHET BAS UNIQUEMENT + trail réactif ─────────────
        // Seuil = 10% du demi-gap (~25 pts pour gridGap=500).
        // Le BS descend dès que le ask baisse d'au moins 25 pts.
        double minChange = halfGap() / 10.0;
        sp.needsModify = (sp.delta > minChange);
    }
    return sp;
}

} // namespace top4ea
