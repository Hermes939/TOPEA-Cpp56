#pragma once
// ============================================================
//  TOP4EA — intelligence_engine.hpp
//  Raisonnement profond — Hedging mathématique institutionnel pur
//
//  ARCHITECTURE 80/20 INSTITUTIONNELLE :
//  ─────────────────────────────────────────────────────────
//  80% MATHÉMATIQUES GÉOMÉTRIQUES PURES :
//    1. RÉGIME OU — Ornstein-Uhlenbeck mean-reversion
//    2. NET DELTA INSTITUTIONNEL — multi-niveau (portfolio + flux)
//    3. IMBALANCE GÉOMÉTRIQUE — FVG + OFI institutionnel
//    4. ASYMÉTRIE GÉOMÉTRIQUE PURE — trail adaptatif par régime
//    5. SCORE DE RISQUE COMPOSITE — 4 axes mathématiques
//
//  20% INDICATEURS + AI :
//    6. ATR DYNAMIQUE — régime de volatilité
//    7. MOMENTUM EWMA — fast/slow divergence
//    8. PRÉDICTION P&L VELOCITY — fermeture prédictive
//    9. AutoML PyTorch — optimisation externe des paramètres
//
//  NOUVEAU — MODULES INSTITUTIONNELS :
//  ─────────────────────────────────────────────────────────
//  GeometricImbalance : FVG + OFI pour ancrage de grille
//  InstitutionalDelta : CVD proxy + divergence + épuisement
//  ErrorMemory        : mémoire des erreurs mathématiques
//  MathCalibrator     : auto-calibration par Adam + AutoML
// ============================================================
#include "types.hpp"
#include "math_core.hpp"
#include "geometric_imbalance.hpp"
#include "institutional_delta.hpp"
#include "error_memory.hpp"
#include "math_calibrator.hpp"
#include "deepseek_vision.hpp"
#include "dxy_analyzer.hpp"
#include <string>
#include <vector>
#include <deque>
#include <mutex>

namespace top4ea {

// ============================================================
//  DirectionVote — résultat du vote board directionnel [-5,+5]
//  Score ≤ ssOnlyThreshold → SELL-only (SS uniquement)
//  Score ≥ bsOnlyThreshold → BUY-only (BS uniquement)
// ============================================================
struct DirectionVote {
    int         score   = 0;        // [-5, +5]
    bool        ssOnly  = false;    // score ≤ -2 → ne placer que SS
    bool        bsOnly  = false;    // score ≥ +2 → ne placer que BS
    std::string reason;             // description lisible pour le log
};

// ─── Vote board directionnel (fonction utilitaire publique) ──
// Agrège : delta lots (±2) + DeepSeek signal (±2) +
//          structure/momentum (±1) + DXY proxy (±1) = [-5,+5]
// lotsStale (Task #17) : si true, la composante delta lots (±2) est
// exclue du score (poids de repli = 0) car buyLots/sellLots proviennent
// d'un cache non rafraîchi depuis trop longtemps (coupure de flux de
// ticks) — on préfère ignorer ce signal plutôt que voter sur une
// exposition potentiellement obsolète.
DirectionVote computeDirectionVote(
    double       buyLots,
    double       sellLots,
    double       totalLots,
    DSSignal     dsSignal,
    double       dsEffectiveConf,
    MarketStructure structure,
    DXYSignal    dxySignal,
    bool         lotsStale       = false,
    int          ssOnlyThreshold = -2,   // score ≤ seuil → SS-only
    int          bsOnlyThreshold =  2,   // score ≥ seuil → BS-only
    int          dsConfHigh      = 60,   // conf DS% → poids ±2 / gate override
    int          dsConfLow       = 40) noexcept;  // conf DS% → poids ±1

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

        // === [80%] Régime de marché (OU + Géométrie) ===
        Regime       regime          = Regime::Unknown;
        double       rho1            = 0.0;   // autocorrélation lag-1 (Ornstein-Uhlenbeck)
        double       momentumFast    = 0.0;   // momentum court terme (EWMA α=0.30)
        double       momentumSlow    = 0.0;   // momentum long terme  (EWMA α=0.03)
        VolRegime    volRegime       = VolRegime::Medium;
        double       atrPoints       = 0.0;

        // === [80%] Delta Institutionnel Multi-Niveau ===
        double       buyLots         = 0.0;
        double       sellLots        = 0.0;
        int          buyOrders       = 0;
        int          sellOrders      = 0;
        double       totalLots       = 0.0;
        double       netDelta        = 0.0;   // BUY lots - SELL lots (portefeuille)
        double       hedgePressure   = 0.0;   // |delta| / totalLots  ∈ [0,1]
        bool         needsHedge      = false; // signal de couverture requis
        int          hedgeType       = -1;    // 0=BUY, 1=SELL  (-1=aucun)
        double       hedgeLot        = 0.0;   // lot pour neutraliser le delta

        // Delta institutionnel flux (CVD proxy)
        double       flowDeltaNet    = 0.0;   // fast - slow EWMA flux
        double       cumulativeDelta = 0.0;   // CVD normalisé [-1,+1]
        DeltaSignal  deltaSignal     = DeltaSignal::Neutral;  // signal institutionnel
        bool         deltaExhaustion = false; // épuisement institutionnel → pivot

        // === [80%] Imbalance Géométrique (FVG + OFI) ===
        double       imbalanceScore  = 0.0;   // score d'imbalance [0,100]
        double       ofiNormalized   = 0.0;   // OFI normalisé [-1,+1]
        double       ssAnchorFVG     = 0.0;   // ancre SellStop sur FVG (0=non dispo)
        double       bsAnchorFVG     = 0.0;   // ancre BuyStop sur FVG (0=non dispo)
        bool         displacementActive = false; // déplacement impulsif actif
        double       asymmetryMult   = 1.0;   // multiplicateur OFI pour asymétrie

        // === [80%] Trail Stop Asymétrique Institutionnel ===
        double       ssGapFactor     = 1.0;   // multiplicateur gap BuyStop  (SS)
        double       bsGapFactor     = 1.0;   // multiplicateur gap SellStop (BS)
        bool         trailFrozen     = false; // stops figés (zone TP proche)
        double       trailTightness  = 1.0;   // serrage global

        // === [80%] Score de risque composite (4 axes) ===
        double       riskScore       = 0.0;   // 0=sécurisé, 100=danger critique
        double       drawdownUSD     = 0.0;
        double       drawdownPct     = 0.0;
        bool         marginAlert     = false;
        bool         cascadeRisk     = false;

        // === [20%] Prédiction convergence (velocity) ===
        double       closeProbPct    = 0.0;
        int          estimatedTicksTarget = 9999;
        double       pnlVelocity     = 0.0;

        // === Calibration auto (paramètres courants) ===
        CalibrableParams calibratedParams;   // paramètres θ courants

        // === Raisonnement structuré complet ===
        std::string  reasoning;
        std::string  deltaReasoning;
        std::string  imbalanceReasoning;
    };

    // ─── Construction ─────────────────────────────────────────
    //     Config passé par valeur (copie) — durée de vie garantie
    explicit IntelligenceEngine(Config cfg);
    ~IntelligenceEngine();

    // ─── Alimentation tick (indicateurs internes) ─────────────
    void onTick(const Tick& tick) noexcept;

    // ─── Alimentation barre OHLC (détection FVG) ─────────────
    void onBar(const OHLCBar& bar) noexcept;

    // ─── Analyse complète (point d'entrée principal) ──────────
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

    // ─── Enregistrement d'un cycle fermé (auto-apprentissage) ─
    //  Appelé par TOP4EA_Engine après chaque fermeture de basket
    void recordCycleClosed(
        double pnlResult,
        double targetExpected,
        double drawdownPct,
        double gapUsedPoints,
        double gapOptimalPoints,
        int    recovDepth,
        double lotUsed,
        // Métriques Dual Trail SELL-side (BS AGGRESSIVE)
        int    trailModeUsed     = 0,   // 0=NATURAL, 1=AGGRESSIVE
        int    vLockTriggered    = 0,   // 1 si verrou BS actif au close du cycle
        double vLockPrice        = 0.0, // niveau verrou BS (0.0 si inactif)
        double ssProfitAtLockPts = 0.0, // profit Sell flottant en pts au lock
        // Métriques Dual Trail BUY-side (SS AGGRESSIVE miroir)
        int    trailModeBuyUsed  = 0,   // 0=NATURAL, 1=AGGRESSIVE
        int    ssLockTriggered   = 0,   // 1 si verrou SS BUY actif au close du cycle
        double ssLockPrice       = 0.0, // niveau verrou SS BUY (0.0 si inactif)
        double bsProfitAtLockPts = 0.0  // profit Buy flottant en pts au lock
    ) noexcept;

    // ─── Application des paramètres calibrés à la config ─────
    void applyCalibration(Config& cfg) const noexcept {
        calibrator_.applyToConfig(cfg);
    }

    // ─── Accès paramètres calibrés courants ──────────────────
    [[nodiscard]] CalibrableParams currentCalibration() const noexcept {
        return calibrator_.currentParams();
    }

    // ─── Rapport d'auto-calibration ──────────────────────────
    [[nodiscard]] std::string calibrationReport() const {
        return calibrator_.report() + "\n" + errorMem_.report();
    }

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

    // ─── Fusion des facteurs gap (80/20) ─────────────────────
    //  80% = mathématiques géométriques (OU + delta + imbalance)
    //  20% = indicateurs (ATR + momentum)
    [[nodiscard]] std::pair<double,double> fuseGapFactors(
        std::pair<double,double> regimeFact,
        std::pair<double,double> deltaFact,
        double                   imbalanceMult,
        double                   pnlRatio) const noexcept;

    // ─── Membres core ─────────────────────────────────────────
    Config                cfg_;
    math::OUEstimator     ouEst_;
    math::VolatilityRegime volReg_;
    math::EWMA            momentumFast_ { 0.30 };
    math::EWMA            momentumSlow_ { 0.03 };
    math::PnLVelocity     pnlVel_       { 0.30 };

    // ─── Membres institutionnels (80% mathématiques) ──────────
    GeometricImbalance    geoImbalance_;   // FVG + OFI
    InstitutionalDelta    instDelta_;      // CVD proxy + divergence

    // ─── Membres auto-apprentissage ───────────────────────────
    ErrorMemory           errorMem_;       // mémoire des erreurs
    MathCalibrator        calibrator_;     // auto-calibration Adam + AutoML

    double                lastMid_      = 0.0;
    double                lastPnL_      = 0.0;

    // Compteur de cycles pour calibration périodique
    int                   cycleCount_   = 0;
    static constexpr int  CALIB_EVERY   = 5;   // calibrer tous les 5 cycles

    mutable std::mutex    mu_;
    Regime                lastRegime_   = Regime::Unknown;
};

} // namespace top4ea
