// ============================================================
//  TOP4EA — geometric_imbalance.cpp
//  Détection d'imbalance géométrique institutionnelle
//  FVG + OFI + Displacement
// ============================================================
#include "../include/geometric_imbalance.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace top4ea {

// ============================================================
//  FVGDetector
// ============================================================

void FVGDetector::pushBar(const OHLCBar& bar) noexcept {
    std::lock_guard lk(mu_);

    // Mise à jour ATR (utilise range de chaque barre)
    double prevClose = bars_.empty() ? bar.open : bars_.back().close;
    atr_.update(bar.high, bar.low, prevClose);

    bars_.push_back(bar);
    if ((int)bars_.size() > 10)
        bars_.pop_front();

    if ((int)bars_.size() < MIN_BARS_FOR_FVG) return;

    // Vieillissement des zones existantes
    ageBars();

    // Détection FVG sur les 3 dernières barres
    // b0 = t-2, b1 = t-1 (barre impulsive), b2 = t (barre courante)
    auto it = bars_.end();
    const OHLCBar& b2 = *(--it);
    const OHLCBar& b1 = *(--it);
    const OHLCBar& b0 = *(--it);

    detectFVG(b0, b1, b2);
    pruneOldZones();
}

void FVGDetector::detectFVG(const OHLCBar& b0,
                              const OHLCBar& b1,
                              const OHLCBar& b2) noexcept
{
    double avgATRval = atr_.value();
    if (avgATRval <= 0.0) return;

    // ── FVG HAUSSIER (bullish FVG) ───────────────────────────
    // b1 est une barre impulsive haussière
    // Il existe un gap entre high[b0] et low[b2]
    // → La zone [high(b0), low(b2)] est un FVG bullish
    if (b1.bullish() && b2.low > b0.high) {
        double gapSize = b2.low - b0.high;
        if (gapSize >= avgATRval * MIN_GAP_ATR) {
            ImbalanceZone z;
            z.type       = ImbalanceZone::Type::FVG_BULLISH;
            z.priceLow   = b0.high;
            z.priceHigh  = b2.low;
            z.midpoint   = (z.priceLow + z.priceHigh) * 0.5;
            z.strength   = std::min(1.0, gapSize / (avgATRval * 3.0));
            z.age        = 0;
            z.tested     = false;
            z.mitigated  = false;

            if ((int)zones_.size() >= maxZones_)
                zones_.pop_front();
            zones_.push_back(z);
        }
    }

    // ── FVG BAISSIER (bearish FVG) ───────────────────────────
    // b1 est une barre impulsive baissière
    // Il existe un gap entre low[b0] et high[b2]
    // → La zone [high(b2), low(b0)] est un FVG bearish
    if (b1.bearish() && b2.high < b0.low) {
        double gapSize = b0.low - b2.high;
        if (gapSize >= avgATRval * MIN_GAP_ATR) {
            ImbalanceZone z;
            z.type       = ImbalanceZone::Type::FVG_BEARISH;
            z.priceLow   = b2.high;
            z.priceHigh  = b0.low;
            z.midpoint   = (z.priceLow + z.priceHigh) * 0.5;
            z.strength   = std::min(1.0, gapSize / (avgATRval * 3.0));
            z.age        = 0;
            z.tested     = false;
            z.mitigated  = false;

            if ((int)zones_.size() >= maxZones_)
                zones_.pop_front();
            zones_.push_back(z);
        }
    }
}

void FVGDetector::updateMitigation(double currentPrice) noexcept {
    std::lock_guard lk(mu_);
    for (auto& z : zones_) {
        if (z.mitigated) continue;
        // Une zone bullish est comblée si le prix ferme en dessous de son midpoint
        if (z.isBullish() && currentPrice < z.midpoint)
            z.mitigated = true;
        // Une zone bearish est comblée si le prix monte au dessus de son midpoint
        if (z.isBearish() && currentPrice > z.midpoint)
            z.mitigated = true;
        // Marquer comme testée si le prix entre dans la zone
        if (!z.tested && z.contains(currentPrice))
            z.tested = true;
    }
}

void FVGDetector::ageBars() noexcept {
    for (auto& z : zones_) ++z.age;
}

void FVGDetector::pruneOldZones() noexcept {
    while (!zones_.empty() && (!zones_.front().valid() || zones_.front().age > 100))
        zones_.pop_front();
}

std::optional<ImbalanceZone> FVGDetector::nearestBullish(double price) const noexcept {
    std::lock_guard lk(mu_);
    std::optional<ImbalanceZone> best;
    double bestDist = 1e18;

    for (const auto& z : zones_) {
        if (!z.valid() || !z.isBullish()) continue;
        if (z.priceHigh >= price) continue;  // doit être en dessous du prix

        double dist = price - z.midpoint;
        if (dist < bestDist) {
            bestDist = dist;
            best     = z;
        }
    }
    return best;
}

std::optional<ImbalanceZone> FVGDetector::nearestBearish(double price) const noexcept {
    std::lock_guard lk(mu_);
    std::optional<ImbalanceZone> best;
    double bestDist = 1e18;

    for (const auto& z : zones_) {
        if (!z.valid() || !z.isBearish()) continue;
        if (z.priceLow <= price) continue;  // doit être au dessus du prix

        double dist = z.midpoint - price;
        if (dist < bestDist) {
            bestDist = dist;
            best     = z;
        }
    }
    return best;
}

// ============================================================
//  OFICalculator
// ============================================================

void OFICalculator::pushTick(const Tick& tick) noexcept {
    if (prevBid_ <= 0.0) {
        prevBid_ = tick.bid;
        prevAsk_ = tick.ask;
        return;
    }

    // Proxy OFI : sens du mouvement bid × amplitude relative
    double bidMove = tick.bid - prevBid_;
    double askMove = tick.ask - prevAsk_;
    double spread  = std::max(1e-10, tick.spread);

    // Pression acheteuse : bid monte (liftés par des acheteurs)
    double buyPress  = (bidMove > 0.0) ? bidMove / spread : 0.0;
    // Pression vendeuse : ask baisse (hitée par des vendeurs)
    double sellPress = (askMove < 0.0) ? -askMove / spread : 0.0;

    double tickOFI = buyPress - sellPress;
    cumOFI_       += tickOFI;
    maxOFI_        = std::max(maxOFI_, std::abs(cumOFI_));

    ofiHistory_.push_back(tickOFI);
    if ((int)ofiHistory_.size() > window_)
        ofiHistory_.pop_front();

    // Normalisation dynamique ∈ [-1, +1]
    double normOFI = (maxOFI_ > 1e-12) ? cumOFI_ / maxOFI_ : 0.0;
    normOFI        = std::clamp(normOFI, -1.0, 1.0);

    double prevEwma = ofiEwma_.value();
    ofiEwma_.update(normOFI);
    double momentum = ofiEwma_.value() - prevEwma;
    ofiMomEwma_.update(momentum);

    buyPressEwma_.update(buyPress);
    sellPressEwma_.update(sellPress);

    prevBid_ = tick.bid;
    prevAsk_ = tick.ask;
}

OFISnapshot OFICalculator::snapshot() const noexcept {
    OFISnapshot s;
    s.cumulativeOFI  = cumOFI_;
    s.normalizedOFI  = ofiEwma_.value();
    s.ofiMomentum    = ofiMomEwma_.value();
    s.buyPressure    = std::max(0.0, ofiEwma_.value());
    s.sellPressure   = std::max(0.0, -ofiEwma_.value());

    double totalPress = s.buyPressure + s.sellPressure;
    if (totalPress > 1e-9) {
        s.imbalanceRatio = std::max(s.buyPressure, s.sellPressure) / totalPress;
    }

    static constexpr double DOM_THRESHOLD = 0.30;
    s.dominantBuy  = (s.normalizedOFI >  DOM_THRESHOLD);
    s.dominantSell = (s.normalizedOFI < -DOM_THRESHOLD);

    return s;
}

void OFICalculator::reset() noexcept {
    prevBid_   = 0.0;
    prevAsk_   = 0.0;
    cumOFI_    = 0.0;
    maxOFI_    = 1e-9;
    ofiHistory_.clear();
    ofiEwma_.reset();
    ofiMomEwma_.reset();
    buyPressEwma_.reset();
    sellPressEwma_.reset();
}

// ============================================================
//  GeometricImbalance
// ============================================================

void GeometricImbalance::pushBar(const OHLCBar& bar) noexcept {
    std::lock_guard lk(mu_);

    double prevClose = recentBars_.empty() ? bar.open : recentBars_.back().close;
    atr_.update(bar.high, bar.low, prevClose);

    recentBars_.push_back(bar);
    if ((int)recentBars_.size() > 5)
        recentBars_.pop_front();

    // Détection displacement
    if (detectDisplacement(bar)) {
        displacementActive_ = true;
        lastDisplaceMag_    = bar.range() / std::max(1e-9, atr_.value());
        lastDisplaceBull_   = bar.bullish();
    } else {
        // L'effet du displacement décroît progressivement
        displacementActive_ = false;
    }

    fvgDetector_.pushBar(bar);
}

void GeometricImbalance::pushTick(const Tick& tick) noexcept {
    ofiCalc_.pushTick(tick);
    fvgDetector_.updateMitigation((tick.bid + tick.ask) * 0.5);
}

bool GeometricImbalance::detectDisplacement(const OHLCBar& bar) const noexcept {
    double atrVal = atr_.value();
    if (atrVal <= 0.0) return false;
    // Displacement : corps de barre ≥ 2.5× ATR
    return (bar.body() >= atrVal * 2.5);
}

ImbalanceAnalysis GeometricImbalance::analyze(double currentPrice) const noexcept {
    ImbalanceAnalysis result;

    result.activeZones = fvgDetector_.activeZones();
    result.nearestBullish = fvgDetector_.nearestBullish(currentPrice);
    result.nearestBearish = fvgDetector_.nearestBearish(currentPrice);
    result.ofi            = ofiCalc_.snapshot();

    // Displacement
    {
        std::lock_guard lk(mu_);
        result.displacementDetected  = displacementActive_;
        result.displacementMagnitude = lastDisplaceMag_;
        result.displacementBullish   = lastDisplaceBull_;
    }

    // ── Ancres suggérées pour la grille ───────────────────────
    if (result.nearestBullish.has_value()) {
        // SS ancré sur la borne basse du FVG bullish (zone de support)
        result.ssAnchorPrice = result.nearestBullish->priceLow;
    }
    if (result.nearestBearish.has_value()) {
        // BS ancré sur la borne haute du FVG bearish (zone de résistance)
        result.bsAnchorPrice = result.nearestBearish->priceHigh;
    }

    // ── Multiplicateur d'asymétrie OFI ────────────────────────
    double absOFI = std::abs(result.ofi.normalizedOFI);
    result.asymmetryMultiplier = 1.0 + absOFI * 0.30;  // max +30% d'extension

    // ── Score d'imbalance global [0,100] ──────────────────────
    double zoneScore  = std::min(1.0, (double)result.activeZones.size() / 5.0);
    double ofiScore   = absOFI;
    double displScore = result.displacementDetected ? 0.3 : 0.0;
    result.imbalanceScore = std::min(100.0,
        (zoneScore * 40.0 + ofiScore * 40.0 + displScore * 20.0));

    // ── Résumé ────────────────────────────────────────────────
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "IMBALANCE: score=" << result.imbalanceScore
        << " zones=" << result.activeZones.size()
        << " OFI=" << result.ofi.normalizedOFI;
    if (result.nearestBullish) oss << " FVG_BULL@" << result.nearestBullish->midpoint;
    if (result.nearestBearish) oss << " FVG_BEAR@" << result.nearestBearish->midpoint;
    if (result.displacementDetected)
        oss << " DISPLACEMENT×" << result.displacementMagnitude
            << (result.displacementBullish ? " ↑" : " ↓");
    result.summary = oss.str();

    return result;
}

double GeometricImbalance::suggestSSAnchor(double currentPrice) const noexcept {
    auto zone = fvgDetector_.nearestBullish(currentPrice);
    return zone.has_value() ? zone->priceLow : 0.0;
}

double GeometricImbalance::suggestBSAnchor(double currentPrice) const noexcept {
    auto zone = fvgDetector_.nearestBearish(currentPrice);
    return zone.has_value() ? zone->priceHigh : 0.0;
}

double GeometricImbalance::asymmetryMultiplier() const noexcept {
    double absOFI = std::abs(ofiCalc_.normalizedPressure());
    return 1.0 + absOFI * 0.30;
}

bool GeometricImbalance::displacementActive() const noexcept {
    std::lock_guard lk(mu_);
    return displacementActive_;
}

double GeometricImbalance::displacementMagnitude() const noexcept {
    std::lock_guard lk(mu_);
    return lastDisplaceMag_;
}

} // namespace top4ea
