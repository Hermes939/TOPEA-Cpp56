#pragma once
// ============================================================
//  TOP4EA — institutional_delta.hpp
//  Delta institutionnel multi-niveau et multi-timeframe
//
//  THÉORIE (Hedge Institutionnel Pur) :
//  ─────────────────────────────────────────────────────────
//  Le "delta institutionnel" n'est PAS le delta options.
//  C'est la mesure de la PRESSION DIRECTIONNELLE NETTE des
//  grandes mains sur le marché à travers plusieurs niveaux :
//
//  NIVEAU 1 — Delta Portfolio (positions ouvertes)
//    δ₁ = Σ(BUY lots) - Σ(SELL lots)
//    → Exposition directionnelle nette du SYSTÈME
//
//  NIVEAU 2 — Delta de Flux (mouvement prix × amplitude)
//    δ₂ = Σ (Δbid > 0 ? +1 : -1) × |Δbid| / spread
//    → Momentum institutionnel tick par tick
//    → Estimé sans volume (MT4 ne donne pas le volume réel)
//
//  NIVEAU 3 — Delta Cumulatif Pondéré (CVD proxy)
//    δ₃ = EWMA(δ₂, α_lent) - EWMA(δ₂, α_rapide)
//    → Divergence lent/rapide = changement de régime institutionnel
//
//  NIVEAU 4 — Delta Acceleration
//    δ₄ = dδ₃/dt = dérivée du delta cumulatif
//    → Accélération de la pression institutionnelle
//    → Anticipe les pivots avant qu'ils se produisent
//
//  SIGNAUX DÉRIVÉS :
//  ─────────────────────────────────────────────────────────
//  DIVERGENCE_BULLISH : prix baisse + delta monte → accumulation
//  DIVERGENCE_BEARISH : prix monte + delta baisse → distribution
//  DELTA_EXHAUSTION   : delta fort mais s'essoufle → pivot imminent
//  DELTA_BREAKOUT     : delta franchit son niveau max historique
//
//  IMPACT SUR LA GRILLE :
//  ─────────────────────────────────────────────────────────
//  - Divergence bullish → protéger côté BS, élargir SS
//  - Divergence bearish → protéger côté SS, élargir BS
//  - Exhaustion → geler trail, attendre le retournement
//  - Breakout → étendre la grille dans le sens du delta
// ============================================================
#include "types.hpp"
#include "math_core.hpp"
#include <deque>
#include <mutex>
#include <cmath>
#include <string>
#include <algorithm>
#include <numeric>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  Signaux delta institutionnels
// ────────────────────────────────────────────────────────────
enum class DeltaSignal : uint8_t {
    Neutral            = 0,
    DivergenceBullish  = 1,   // accumulation institutionnelle
    DivergenceBearish  = 2,   // distribution institutionnelle
    DeltaExhaustion    = 3,   // épuisement de la pression
    DeltaBreakoutBull  = 4,   // percée haussière du delta
    DeltaBreakoutBear  = 5    // percée baissière du delta
};

inline const char* deltaSignalName(DeltaSignal s) noexcept {
    switch (s) {
        case DeltaSignal::DivergenceBullish: return "DIV_BULL (accumulation)";
        case DeltaSignal::DivergenceBearish: return "DIV_BEAR (distribution)";
        case DeltaSignal::DeltaExhaustion:   return "EXHAUSTION (pivot)";
        case DeltaSignal::DeltaBreakoutBull: return "BREAKOUT_BULL";
        case DeltaSignal::DeltaBreakoutBear: return "BREAKOUT_BEAR";
        default:                             return "NEUTRAL";
    }
}

// ────────────────────────────────────────────────────────────
//  Snapshot delta institutionnel complet
// ────────────────────────────────────────────────────────────
struct DeltaSnapshot {
    // Niveau 1 — Portfolio
    double portfolioDelta    = 0.0;   // BUY lots - SELL lots
    double portfolioPressure = 0.0;   // |delta| / totalLots ∈ [0,1]

    // Niveau 2 — Flux de marché (proxy CVD)
    double flowDeltaFast     = 0.0;   // EWMA rapide du flux
    double flowDeltaSlow     = 0.0;   // EWMA lent du flux
    double flowDeltaNet      = 0.0;   // fast - slow (divergence interne)

    // Niveau 3 — Delta cumulatif
    double cumulativeDelta   = 0.0;   // CVD proxy normalisé [-1, +1]
    double deltaPercentile   = 0.0;   // rang percentile du CVD courant [0,1]

    // Niveau 4 — Accélération
    double deltaAcceleration = 0.0;   // dérivée du cumul delta
    double accelerationEwma  = 0.0;   // EWMA de l'accélération

    // Divergence prix/delta
    bool   divergenceBull    = false; // prix ↓ + delta ↑
    bool   divergenceBear    = false; // prix ↑ + delta ↓
    double divergenceScore   = 0.0;   // force de la divergence [0,1]

    // Épuisement
    bool   exhaustion        = false; // delta fort mais décélère
    double exhaustionScore   = 0.0;

    // Signal primaire
    DeltaSignal primarySignal = DeltaSignal::Neutral;
    double      signalStrength = 0.0;  // ∈ [0,1]

    // Facteurs d'ajustement de grille suggérés
    double suggestedSSFactor = 1.0;   // multiplicateur gap BuyStop
    double suggestedBSFactor = 1.0;   // multiplicateur gap SellStop
    bool   suggestTrailFreeze = false; // geler le trail

    std::string reasoning;
};

// ============================================================
//  InstitutionalDelta — Tracker delta multi-niveau
// ============================================================
class InstitutionalDelta {
public:
    static constexpr int  CVD_HISTORY      = 200;  // fenêtre historique CVD
    static constexpr int  PRICE_HISTORY    = 50;   // fenêtre divergence prix
    static constexpr double DIV_THRESHOLD  = 0.25; // seuil divergence significative
    static constexpr double EXHAUST_DECAY  = 0.30; // décroissance pour épuisement

    explicit InstitutionalDelta()
        : flowFast_(0.30)
        , flowSlow_(0.03)
        , cvdEwma_(0.10)
        , accelEwma_(0.20)
        , priceFast_(0.30)
        , priceSlow_(0.03)
    {}

    // ─── Alimentation tick de marché ──────────────────────────
    void pushTick(const Tick& tick) noexcept;

    // ─── Alimentation positions (Niveau 1) ───────────────────
    void updatePortfolio(double buyLots, double sellLots,
                         int buyOrders, int sellOrders) noexcept;

    // ─── Analyse complète ─────────────────────────────────────
    [[nodiscard]] DeltaSnapshot analyze() const noexcept;

    // ─── Facteurs d'ajustement rapides (en-ligne, <1µs) ──────
    [[nodiscard]] double quickSSFactor() const noexcept;
    [[nodiscard]] double quickBSFactor() const noexcept;

    // ─── Accès état ───────────────────────────────────────────
    [[nodiscard]] double cumulativeDelta() const noexcept {
        return cvdEwma_.value();
    }
    [[nodiscard]] DeltaSignal lastSignal() const noexcept {
        return lastSignal_;
    }

    void reset() noexcept;

private:
    // Calcul proxy CVD depuis les mouvements bid/ask
    [[nodiscard]] double computeTickDelta(const Tick& curr,
                                          const Tick& prev) const noexcept;

    // Détection divergence prix/delta
    [[nodiscard]] std::pair<bool,bool> detectDivergence() const noexcept;

    // Détection épuisement
    [[nodiscard]] bool detectExhaustion() const noexcept;

    // Classification du signal primaire
    [[nodiscard]] DeltaSignal classifySignal(
        bool divBull, bool divBear,
        bool exhaust,
        double cvd) const noexcept;

    // Construction du raisonnement
    [[nodiscard]] std::string buildReasoning(const DeltaSnapshot& s) const;

    // Facteurs de grille suggérés
    [[nodiscard]] std::pair<double,double> computeGridFactors(
        DeltaSignal signal,
        double cvd,
        double portfolioPressure) const noexcept;

    // ── Niveau 1 ─────────────────────────────────────────────
    double buyLots_          = 0.0;
    double sellLots_         = 0.0;
    int    buyOrders_        = 0;
    int    sellOrders_       = 0;

    // ── Niveau 2 ─────────────────────────────────────────────
    math::EWMA flowFast_;       // alpha=0.30 → flux rapide
    math::EWMA flowSlow_;       // alpha=0.03 → flux lent

    // ── Niveau 3 ─────────────────────────────────────────────
    math::EWMA cvdEwma_;        // CVD lissé
    std::deque<double> cvdHistory_;   // historique pour percentile

    // ── Niveau 4 ─────────────────────────────────────────────
    math::EWMA accelEwma_;      // accélération du delta

    // ── Prix ─────────────────────────────────────────────────
    math::EWMA priceFast_;
    math::EWMA priceSlow_;
    std::deque<double> priceHistory_;

    // ── État ─────────────────────────────────────────────────
    Tick           prevTick_;
    bool           hasPrev_    = false;
    double         prevCVD_    = 0.0;
    DeltaSignal    lastSignal_ = DeltaSignal::Neutral;
    mutable std::mutex mu_;
};

} // namespace top4ea
