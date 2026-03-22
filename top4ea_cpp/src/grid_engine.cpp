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
{}

void GridEngine::onTick(const Tick& tick) noexcept {
    std::lock_guard lk(mu_);
    lastBid_ = tick.bid;
    lastAsk_ = tick.ask;
    spreadTrack_.push(tick.spread);

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

int GridEngine::effectiveGridGap() const noexcept {
    if (!cfg_.useAtrGrid || !atr_.ready())
        return cfg_.gridGap;
    return math::dynamicGridGap(atrPoints(),
                                cfg_.atrMultiplier,
                                cfg_.atrGridMin,
                                cfg_.atrGridMax);
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
    double minDist) const noexcept
{
    std::lock_guard lk(mu_);
    StopPlacement sp;
    double hg    = halfGap();
    double mdist = (minDist > 0.0) ? minDist : minDistPts();
    double target = math::normalize(lastBid_ - hg, cfg_.digits);
    if ((lastBid_ - target) < mdist)
        target = math::normalize(lastBid_ - mdist, cfg_.digits);

    sp.targetPrice  = target;
    sp.currentPrice = currentLevel;

    if (currentTicket <= 0) {
        sp.needsCreate = true;
    } else {
        sp.delta = target - currentLevel;
        // Déplace uniquement si la cible s'est éloignée significativement
        // (> 2 points vers le bas = conditions plus favorables)
        sp.needsModify = (target > currentLevel + 2.0 * pointSize());
    }
    return sp;
}

StopPlacement GridEngine::computeBuyStop(
    int    currentTicket,
    double currentLevel,
    double minDist) const noexcept
{
    std::lock_guard lk(mu_);
    StopPlacement sp;
    double hg    = halfGap();
    double mdist = (minDist > 0.0) ? minDist : minDistPts();
    double target = math::normalize(lastAsk_ + hg, cfg_.digits);
    if ((target - lastAsk_) < mdist)
        target = math::normalize(lastAsk_ + mdist, cfg_.digits);

    sp.targetPrice  = target;
    sp.currentPrice = currentLevel;

    if (currentTicket <= 0) {
        sp.needsCreate = true;
    } else {
        sp.delta = currentLevel - target;
        // Déplace si cible descend (conditions plus favorables)
        sp.needsModify = (target < currentLevel - 2.0 * pointSize());
    }
    return sp;
}

} // namespace top4ea
