// ============================================================
//  TOP4EA — institutional_delta.cpp
//  Delta institutionnel multi-niveau
//  CVD proxy + divergence + épuisement
// ============================================================
#include "../include/institutional_delta.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <numeric>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  Mise à jour portfolio (Niveau 1)
// ────────────────────────────────────────────────────────────
void InstitutionalDelta::updatePortfolio(double buyLots, double sellLots,
                                          int buyOrders, int sellOrders) noexcept {
    std::lock_guard lk(mu_);
    buyLots_    = buyLots;
    sellLots_   = sellLots;
    buyOrders_  = buyOrders;
    sellOrders_ = sellOrders;
}

// ────────────────────────────────────────────────────────────
//  Alimentation tick (Niveaux 2-4)
// ────────────────────────────────────────────────────────────
void InstitutionalDelta::pushTick(const Tick& tick) noexcept {
    std::lock_guard lk(mu_);

    if (!hasPrev_) {
        prevTick_ = tick;
        hasPrev_  = true;
        return;
    }

    // Calcul du delta tick
    double tickDelta = computeTickDelta(tick, prevTick_);

    // Niveau 2 : flux fast/slow
    flowFast_.update(tickDelta);
    flowSlow_.update(tickDelta);

    // Niveau 3 : CVD cumulatif (intégrale des flux)
    double prevCVD = prevCVD_;
    double newCVD  = prevCVD * 0.99 + tickDelta * 0.01;  // décroissance lente
    prevCVD_       = newCVD;
    cvdEwma_.update(newCVD);

    cvdHistory_.push_back(newCVD);
    if ((int)cvdHistory_.size() > CVD_HISTORY)
        cvdHistory_.pop_front();

    // Niveau 4 : accélération
    double accel = newCVD - prevCVD;
    accelEwma_.update(accel);

    // Prix
    double mid = (tick.bid + tick.ask) * 0.5;
    priceFast_.update(mid);
    priceSlow_.update(mid);

    priceHistory_.push_back(mid);
    if ((int)priceHistory_.size() > PRICE_HISTORY)
        priceHistory_.pop_front();

    prevTick_ = tick;

    // Mise à jour signal primaire (léger, en-ligne)
    auto [divBull, divBear] = detectDivergence();
    bool exhaust = detectExhaustion();
    lastSignal_  = classifySignal(divBull, divBear, exhaust, newCVD);
}

// ────────────────────────────────────────────────────────────
//  Proxy CVD depuis bid/ask movement
// ────────────────────────────────────────────────────────────
double InstitutionalDelta::computeTickDelta(const Tick& curr,
                                              const Tick& prev) const noexcept
{
    double spread  = std::max(1e-10, curr.spread);
    double bidMove = curr.bid - prev.bid;
    double askMove = curr.ask - prev.ask;

    // Acheteur agressif : bid monte et ask monte → pression acheteuse
    // Vendeur agressif  : bid descend et ask descend → pression vendeuse
    double netMove = (bidMove + askMove) * 0.5;
    return netMove / spread;
}

// ────────────────────────────────────────────────────────────
//  Détection divergence prix/delta
// ────────────────────────────────────────────────────────────
std::pair<bool,bool> InstitutionalDelta::detectDivergence() const noexcept {
    if ((int)priceHistory_.size() < 10 || (int)cvdHistory_.size() < 10)
        return {false, false};

    int n = std::min((int)priceHistory_.size(), (int)cvdHistory_.size());
    int lookback = std::min(n, PRICE_HISTORY);

    // Tendance prix sur les lookback ticks
    double priceFirst = priceHistory_[priceHistory_.size() - lookback];
    double priceLast  = priceHistory_.back();
    double priceTrend = priceLast - priceFirst;

    // Tendance CVD sur le même horizon
    double cvdFirst = cvdHistory_[cvdHistory_.size() - lookback];
    double cvdLast  = cvdHistory_.back();
    double cvdTrend = cvdLast - cvdFirst;

    // Normalisation pour comparaison
    double priceRange = std::max(1e-9, std::abs(priceTrend));
    double cvdRange   = std::max(1e-9, std::abs(cvdTrend));

    bool divBull = (priceTrend < -priceRange * 0.1) &&  // prix baisse
                   (cvdTrend   >  cvdRange   * 0.1);   // delta monte → accumulation

    bool divBear = (priceTrend >  priceRange * 0.1) &&  // prix monte
                   (cvdTrend   < -cvdRange   * 0.1);   // delta baisse → distribution

    return {divBull, divBear};
}

// ────────────────────────────────────────────────────────────
//  Détection épuisement : delta fort mais accélération décroît
// ────────────────────────────────────────────────────────────
bool InstitutionalDelta::detectExhaustion() const noexcept {
    if ((int)cvdHistory_.size() < 20) return false;

    double absCVD  = std::abs(cvdEwma_.value());
    double accel   = accelEwma_.value();

    // Fort CVD mais accélération inverse (décélération)
    bool highCVD   = (absCVD > 0.3);
    bool decel     = (cvdEwma_.value() > 0 && accel < -EXHAUST_DECAY) ||
                     (cvdEwma_.value() < 0 && accel >  EXHAUST_DECAY);

    return highCVD && decel;
}

// ────────────────────────────────────────────────────────────
//  Classification du signal primaire
// ────────────────────────────────────────────────────────────
DeltaSignal InstitutionalDelta::classifySignal(
    bool divBull, bool divBear,
    bool exhaust,
    double cvd) const noexcept
{
    if (exhaust)   return DeltaSignal::DeltaExhaustion;
    if (divBull)   return DeltaSignal::DivergenceBullish;
    if (divBear)   return DeltaSignal::DivergenceBearish;

    // Breakout du CVD si hors du percentile historique
    if ((int)cvdHistory_.size() >= 20) {
        std::vector<double> sorted(cvdHistory_.begin(), cvdHistory_.end());
        std::sort(sorted.begin(), sorted.end());
        double p95 = sorted[(int)(sorted.size() * 0.95)];
        double p05 = sorted[(int)(sorted.size() * 0.05)];

        if (cvd > p95) return DeltaSignal::DeltaBreakoutBull;
        if (cvd < p05) return DeltaSignal::DeltaBreakoutBear;
    }

    return DeltaSignal::Neutral;
}

// ────────────────────────────────────────────────────────────
//  Facteurs de grille depuis le signal delta
// ────────────────────────────────────────────────────────────
std::pair<double,double> InstitutionalDelta::computeGridFactors(
    DeltaSignal signal,
    double cvd,
    double portfolioPressure) const noexcept
{
    double ssF = 1.0;  // facteur BuyStop
    double bsF = 1.0;  // facteur SellStop

    switch (signal) {
        case DeltaSignal::DivergenceBullish:
            // Accumulation → prix va probablement monter
            // Protéger le côté BS (SellStop plus large), SS serré
            bsF = 1.30;
            ssF = 0.85;
            break;
        case DeltaSignal::DivergenceBearish:
            // Distribution → prix va probablement baisser
            // Protéger le côté SS (BuyStop plus large), BS serré
            ssF = 1.30;
            bsF = 0.85;
            break;
        case DeltaSignal::DeltaExhaustion:
            // Épuisement → pivot imminent → geler les deux côtés
            ssF = 1.20;
            bsF = 1.20;
            break;
        case DeltaSignal::DeltaBreakoutBull:
            ssF = 1.40;
            bsF = 0.80;
            break;
        case DeltaSignal::DeltaBreakoutBear:
            ssF = 0.80;
            bsF = 1.40;
            break;
        default:
            break;
    }

    // Ajustement par la pression portfolio (delta portefeuille)
    if (portfolioPressure > 0.3) {
        double adj = portfolioPressure * 0.20;
        ssF *= (1.0 + adj * ((cvd > 0) ? 1.0 : -0.5));
        bsF *= (1.0 + adj * ((cvd < 0) ? 1.0 : -0.5));
    }

    ssF = std::clamp(ssF, 0.60, 2.50);
    bsF = std::clamp(bsF, 0.60, 2.50);

    return {ssF, bsF};
}

// ────────────────────────────────────────────────────────────
//  Analyse complète
// ────────────────────────────────────────────────────────────
DeltaSnapshot InstitutionalDelta::analyze() const noexcept {
    std::lock_guard lk(mu_);
    DeltaSnapshot s;

    // Niveau 1 — Portfolio
    double totalLots = buyLots_ + sellLots_;
    s.portfolioDelta    = buyLots_ - sellLots_;
    s.portfolioPressure = (totalLots > 1e-6)
                        ? std::abs(s.portfolioDelta) / totalLots
                        : 0.0;

    // Niveau 2 — Flux
    s.flowDeltaFast = flowFast_.value();
    s.flowDeltaSlow = flowSlow_.value();
    s.flowDeltaNet  = s.flowDeltaFast - s.flowDeltaSlow;

    // Niveau 3 — CVD
    s.cumulativeDelta = cvdEwma_.value();
    if ((int)cvdHistory_.size() >= 20) {
        std::vector<double> sorted(cvdHistory_.begin(), cvdHistory_.end());
        std::sort(sorted.begin(), sorted.end());
        double rank = 0.0;
        for (double v : sorted) {
            if (v <= s.cumulativeDelta) rank += 1.0;
        }
        s.deltaPercentile = rank / sorted.size();
    }

    // Niveau 4 — Accélération
    s.deltaAcceleration = s.cumulativeDelta - prevCVD_;
    s.accelerationEwma  = accelEwma_.value();

    // Divergence
    auto [divBull, divBear] = detectDivergence();
    s.divergenceBull = divBull;
    s.divergenceBear = divBear;
    s.divergenceScore = (divBull || divBear) ?
        std::min(1.0, std::abs(s.flowDeltaNet) + s.portfolioPressure) : 0.0;

    // Épuisement
    s.exhaustion      = detectExhaustion();
    s.exhaustionScore = s.exhaustion ?
        std::min(1.0, std::abs(s.accelerationEwma) * 3.0) : 0.0;

    // Signal primaire
    s.primarySignal  = classifySignal(divBull, divBear, s.exhaustion, s.cumulativeDelta);
    s.signalStrength = std::max({s.divergenceScore, s.exhaustionScore,
                                  std::min(1.0, std::abs(s.cumulativeDelta))});

    // Gel du trail si épuisement fort
    s.suggestTrailFreeze = (s.exhaustion && s.exhaustionScore > 0.6);

    // Facteurs de grille
    auto [ssF, bsF] = computeGridFactors(s.primarySignal,
                                          s.cumulativeDelta,
                                          s.portfolioPressure);
    s.suggestedSSFactor = ssF;
    s.suggestedBSFactor = bsF;

    // Raisonnement
    s.reasoning = buildReasoning(s);

    return s;
}

// ────────────────────────────────────────────────────────────
//  Facteurs rapides (en-ligne)
// ────────────────────────────────────────────────────────────
double InstitutionalDelta::quickSSFactor() const noexcept {
    DeltaSignal sig = lastSignal_;
    switch (sig) {
        case DeltaSignal::DivergenceBullish:  return 0.85;
        case DeltaSignal::DivergenceBearish:  return 1.25;
        case DeltaSignal::DeltaBreakoutBear:  return 0.80;
        case DeltaSignal::DeltaBreakoutBull:  return 1.35;
        case DeltaSignal::DeltaExhaustion:    return 1.15;
        default:                               return 1.0;
    }
}

double InstitutionalDelta::quickBSFactor() const noexcept {
    DeltaSignal sig = lastSignal_;
    switch (sig) {
        case DeltaSignal::DivergenceBullish:  return 1.25;
        case DeltaSignal::DivergenceBearish:  return 0.85;
        case DeltaSignal::DeltaBreakoutBull:  return 0.80;
        case DeltaSignal::DeltaBreakoutBear:  return 1.35;
        case DeltaSignal::DeltaExhaustion:    return 1.15;
        default:                               return 1.0;
    }
}

// ────────────────────────────────────────────────────────────
//  Raisonnement textuel
// ────────────────────────────────────────────────────────────
std::string InstitutionalDelta::buildReasoning(const DeltaSnapshot& s) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "DELTA INST: "
        << deltaSignalName(s.primarySignal)
        << " | strength=" << s.signalStrength
        << " | CVD=" << s.cumulativeDelta
        << " | P₁=" << s.portfolioDelta << "lots"
        << " | flowNet=" << s.flowDeltaNet;
    if (s.divergenceBull) oss << " | ⬆ ACCUM";
    if (s.divergenceBear) oss << " | ⬇ DISTRIB";
    if (s.exhaustion)     oss << " | ⏸ ÉPUISEMENT";
    if (s.suggestTrailFreeze) oss << " | ❄ GEL TRAIL";
    oss << " → ss×" << s.suggestedSSFactor << " bs×" << s.suggestedBSFactor;
    return oss.str();
}

// ────────────────────────────────────────────────────────────
//  Reset
// ────────────────────────────────────────────────────────────
void InstitutionalDelta::reset() noexcept {
    std::lock_guard lk(mu_);
    flowFast_.reset();
    flowSlow_.reset();
    cvdEwma_.reset();
    accelEwma_.reset();
    priceFast_.reset();
    priceSlow_.reset();
    cvdHistory_.clear();
    priceHistory_.clear();
    prevCVD_    = 0.0;
    hasPrev_    = false;
    lastSignal_ = DeltaSignal::Neutral;
}

} // namespace top4ea
