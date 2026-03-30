#pragma once
// ============================================================
//  TOP4EA — intelligence_engine.hpp
//  Raisonnement profond — Hedging mathématique pur
//
//  PRINCIPES FONDAMENTAUX :
//  ─────────────────────────────────────────────────────────
//  1. RÉGIME DE MARCHÉ — classifie le comportement du prix
//     sur 4 axes : mean-reversion (OU), volatilité (ATR),
//     momentum (EWMA fast/slow), spread qualité.
//
//  2. NET DELTA (HEDGING PUR) — mesure l'exposition nette
//     delta = Σ(BUY lots) - Σ(SELL lots)
//     Idéal delta-neutre = 0. Signal hedge si |delta| > seuil.
//
//  3. TRAIL STOP INTELLIGENT — facteurs adaptatifs
//     ssGapFactor, bsGapFactor modulés par régime + delta
//     pour maximiser la capture de profit et minimiser les
//     cascades de recovery inutiles.
//
//  4. SCORE DE RISQUE — 0 (sécurisé) → 100 (danger critique)
//     Composantes : profondeur recovery, pression marge,
//     déséquilibre delta, qualité de marché.
//
//  5. RAISONNEMENT TEXTUEL — chaque décision est expliquée
//     en clair pour l'opérateur et le tableau de bord.
// ============================================================
#include "types.hpp"
#include "math_core.hpp"
#include <string>
#include <vector>
#include <deque>
#include <mutex>

namespace top4ea {

// ============================================================
//  IntelligenceEngine
// ============================================================
class IntelligenceEngine {
public:

    // ─── Régime de marché multi-dimensionnel ─────────────────
    enum class Regime : uint8_t {
        Unknown        = 0,
        RangeQuiet     = 1,   // mean-reverting, faible vol → grille agressive
        RangeVolatile  = 2,   // mean-reverting, haute vol → stops plus larges
        TrendUp        = 3,   // tendance haussière → risque cascade SS
        TrendDown      = 4,   // tendance baissière → risque cascade BS
        Transitional   = 5    // changement de régime en cours
    };

    // ─── Résultat complet de l'analyse ───────────────────────
    struct Analysis {

        // === Régime de marché ===
        Regime       regime          = Regime::Unknown;
        double       rho1            = 0.0;   // autocorrélation lag-1 (Ornstein-Uhlenbeck)
        double       momentumFast    = 0.0;   // momentum court terme (EWMA α=0.30)
        double       momentumSlow    = 0.0;   // momentum long terme  (EWMA α=0.03)
        VolRegime    volRegime       = VolRegime::Medium;
        double       atrPoints       = 0.0;

        // === Hedging Delta-Neutre Pur ===
        double       buyLots         = 0.0;
        double       sellLots        = 0.0;
        int          buyOrders       = 0;
        int          sellOrders      = 0;
        double       totalLots       = 0.0;
        double       netDelta        = 0.0;   // BUY lots - SELL lots (+>0 = net long)
        double       hedgePressure   = 0.0;   // |delta| / totalLots  ∈ [0,1]
        bool         needsHedge      = false; // signal de couverture requis
        int          hedgeType       = -1;    // 0=BUY, 1=SELL  (-1=aucun)
        double       hedgeLot        = 0.0;   // lot suggéré pour neutraliser le delta

        // === Trail Stop Intelligent ===
        double       ssGapFactor     = 1.0;   // multiplicateur gap BuyStop  (SS)
        double       bsGapFactor     = 1.0;   // multiplicateur gap SellStop (BS)
        bool         trailFrozen     = false; // stops figés (zone TP proche)
        double       trailTightness  = 1.0;   // serrage global (< 1 = plus serré)

        // === Score de risque multidimensionnel ===
        double       riskScore       = 0.0;   // 0=sécurisé, 100=danger critique
        double       drawdownUSD     = 0.0;   // drawdown absolu en $
        double       drawdownPct     = 0.0;   // drawdown en % du capital
        bool         marginAlert     = false; // alerte niveau de marge critique
        bool         cascadeRisk     = false; // risque cascade imminente

        // === Prédiction convergence ===
        double       closeProbPct    = 0.0;   // probabilité de close ≤ horizon ticks
        int          estimatedTicksTarget = 9999; // ticks estimés avant target
        double       pnlVelocity     = 0.0;   // vitesse P&L ($/tick, EWMA)

        // === Raisonnement lisible ===
        std::string  reasoning;
    };

    // ─── Construction ─────────────────────────────────────────
    //     Config passé par valeur (copie) — durée de vie garantie
    explicit IntelligenceEngine(Config cfg);

    // ─── Alimentation tick (indicateurs internes) ─────────────
    void onTick(const Tick& tick) noexcept;

    // ─── Analyse complète ─────────────────────────────────────
    Analysis analyze(
        const std::vector<OrderInfo>& orders,
        const Tick&                   tick,
        const AccountSnapshot&        account,
        double basketPnL,
        double basketTarget,
        double atrPoints,
        int    ssRecovery,
        int    bsRecovery
    );

    // ─── Accès état courant ───────────────────────────────────
    [[nodiscard]] Regime lastRegime() const noexcept {
        std::lock_guard lk(mu_);
        return lastRegime_;
    }

    static const char* regimeName(Regime r) noexcept;

private:
    // ─── Classification régime ───────────────────────────────
    [[nodiscard]] Regime classifyRegime(
        double atrPoints,
        double rho1,
        double momentumFast,
        double momentumSlow) const noexcept;

    // ─── Facteurs adaptatifs trail stop ──────────────────────
    [[nodiscard]] std::pair<double,double> computeGapFactors(
        Regime r,
        double netDelta,
        double totalLots,
        int    ssR,
        int    bsR,
        double pnlRatio) const noexcept;

    // ─── Signal de hedge delta-neutre ────────────────────────
    [[nodiscard]] bool shouldHedge(
        double netDelta,
        double totalLots,
        Regime r,
        double riskScore) const noexcept;

    // ─── Score de risque composite ────────────────────────────
    [[nodiscard]] double computeRiskScore(
        double drawdownPct,
        double marginLevel,
        int    ssR,
        int    bsR,
        double hedgePressure) const noexcept;

    // ─── Construction du texte de raisonnement ────────────────
    [[nodiscard]] std::string buildReasoning(const Analysis& a,
                                              double pnlRatio) const;

    // ─── Membres ─────────────────────────────────────────────
    Config                cfg_;  // copie locale (durée de vie autonome)
    math::OUEstimator     ouEst_;
    math::VolatilityRegime volReg_;
    math::EWMA            momentumFast_ { 0.30 };
    math::EWMA            momentumSlow_ { 0.03 };
    math::PnLVelocity     pnlVel_       { 0.30 };

    double                lastMid_      = 0.0;
    double                lastPnL_      = 0.0;

    mutable std::mutex    mu_;
    Regime                lastRegime_   = Regime::Unknown;
};

} // namespace top4ea
