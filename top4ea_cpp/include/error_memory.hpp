#pragma once
// ============================================================
//  TOP4EA — error_memory.hpp
//  Mémoire structurée des erreurs mathématiques
//
//  PRINCIPES INSTITUTIONNELS :
//  ─────────────────────────────────────────────────────────
//  Chaque erreur de décision est enregistrée avec le contexte
//  mathématique complet. Le système apprend de SES PROPRES
//  erreurs — pas d'une simulation ou d'un backtest externe.
//
//  TYPES D'ERREURS TRACKÉS :
//  ─────────────────────────────────────────────────────────
//  GRID_MISPLACE    : Placement géométrique incorrect
//                     (gap réel ≠ gap théorique OU/ATR)
//  CASCADE_AVOIDABLE: Cascade s'est produite mais aurait pu
//                     être évitée avec un gap plus large
//  LOT_SUBOPTIMAL   : Lot de recovery trop grand ou trop petit
//                     (P&L recovery insuffisant ou risque excessif)
//  HEDGE_MISS       : Signal de hedge raté (delta non neutralisé)
//  TARGET_OVERSHOOT : Basket fermé trop tard (> 110% target)
//  TARGET_MISS      : Target non atteint → retournement du marché
//  IMBALANCE_MISS   : Zone d'imbalance ignorée → cascade
//  ASYMMETRY_ERROR  : Asymétrie appliquée dans le mauvais sens
//
//  AUTO-CORRECTION :
//  ─────────────────────────────────────────────────────────
//  Chaque erreur contribue à un vecteur de correction Δθ sur
//  les paramètres mathématiques :
//    atrMultiplier, kellyFraction, gapExpansionFactor,
//    trailAsymmetryFactor, velocityTrigger
//
//  Algorithme de correction : gradient décroissant à taux
//  adaptatif (Adam simplifié, sans bibliothèque externe).
// ============================================================
#include "types.hpp"
#include "math_core.hpp"
#include <deque>
#include <array>
#include <string>
#include <mutex>
#include <cmath>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  Classification des erreurs mathématiques
// ────────────────────────────────────────────────────────────
enum class ErrorType : uint8_t {
    GRID_MISPLACE     = 0,
    CASCADE_AVOIDABLE = 1,
    LOT_SUBOPTIMAL    = 2,
    HEDGE_MISS        = 3,
    TARGET_OVERSHOOT  = 4,
    TARGET_MISS       = 5,
    IMBALANCE_MISS    = 6,
    ASYMMETRY_ERROR   = 7,
    COUNT             = 8
};

inline const char* errorTypeName(ErrorType t) noexcept {
    switch (t) {
        case ErrorType::GRID_MISPLACE:     return "GRID_MISPLACE";
        case ErrorType::CASCADE_AVOIDABLE: return "CASCADE_AVOIDABLE";
        case ErrorType::LOT_SUBOPTIMAL:    return "LOT_SUBOPTIMAL";
        case ErrorType::HEDGE_MISS:        return "HEDGE_MISS";
        case ErrorType::TARGET_OVERSHOOT:  return "TARGET_OVERSHOOT";
        case ErrorType::TARGET_MISS:       return "TARGET_MISS";
        case ErrorType::IMBALANCE_MISS:    return "IMBALANCE_MISS";
        case ErrorType::ASYMMETRY_ERROR:   return "ASYMMETRY_ERROR";
        default:                           return "UNKNOWN";
    }
}

// ────────────────────────────────────────────────────────────
//  Contexte mathématique complet d'une erreur
// ────────────────────────────────────────────────────────────
struct TradeError {
    ErrorType   type              = ErrorType::GRID_MISPLACE;
    double      severity          = 0.0;   // 0.0 → 1.0 (gravité normalisée)

    // Contexte marché au moment de l'erreur
    double      atrPoints         = 0.0;
    double      atrMultiplierUsed = 0.0;
    double      gapUsedPoints     = 0.0;   // gap effectivement utilisé
    double      gapOptimalPoints  = 0.0;   // gap mathématiquement optimal
    double      rho1              = 0.0;   // autocorrélation OU
    int         regime            = 0;     // Regime enum as int

    // Contexte portfolio
    double      netDelta          = 0.0;
    double      hedgePressure     = 0.0;
    int         recovDepth        = 0;
    double      lotUsed           = 0.0;
    double      lotOptimal        = 0.0;   // lot qui aurait été optimal

    // Résultat
    double      pnlResult         = 0.0;   // P&L réalisé sur ce cycle
    double      targetExpected    = 0.0;   // target attendu
    double      drawdownAtError   = 0.0;   // drawdown en % au moment de l'erreur

    // Correction suggérée (Δθ sur les paramètres)
    double      deltaAtrMult      = 0.0;
    double      deltaKellyFrac    = 0.0;
    double      deltaGapExpansion = 0.0;
    double      deltaTrailAsym    = 0.0;
    double      deltaVelocityTrig = 0.0;

    // Timestamp
    std::chrono::system_clock::time_point ts = std::chrono::system_clock::now();
};

// ────────────────────────────────────────────────────────────
//  Statistiques d'erreur par type
// ────────────────────────────────────────────────────────────
struct ErrorStats {
    int    count        = 0;
    double totalSev     = 0.0;   // somme des sévérités
    double avgSev       = 0.0;   // sévérité moyenne
    double recentTrend  = 0.0;   // tendance récente (EWMA)
    double frequency    = 0.0;   // erreurs / 100 cycles
};

// ────────────────────────────────────────────────────────────
//  Paramètres calibrables (vecteur θ)
// ────────────────────────────────────────────────────────────
struct CalibrableParams {
    double atrMultiplier      = 0.50;
    double kellyMaxFraction   = 0.25;
    double gapExpansionFactor = 0.05;
    double trailAsymmetryFactor = 0.15;
    double velocityTrigger    = 0.85;
    double cppiMultiplier     = 3.0;
    double vabtCoeff          = 0.15;
    double ouMinTheta         = 0.05;

    // Moment d'Adam (accumulateurs gradient)
    double m_atr     = 0.0;  // 1er moment
    double v_atr     = 0.0;  // 2ème moment
    double m_kelly   = 0.0;
    double v_kelly   = 0.0;
    double m_gap     = 0.0;
    double v_gap     = 0.0;
    double m_asym    = 0.0;
    double v_asym    = 0.0;
    double m_vel     = 0.0;
    double v_vel     = 0.0;
    int    t_adam    = 0;    // pas de temps Adam

    void applyToConfig(Config& cfg) const noexcept {
        cfg.atrMultiplier        = atrMultiplier;
        cfg.kellyMaxFraction     = kellyMaxFraction;
        cfg.gapExpansionFactor   = gapExpansionFactor;
        cfg.trailAsymmetryFactor = trailAsymmetryFactor;
        cfg.velocityTrigger      = velocityTrigger;
        cfg.cppiMultiplier       = cppiMultiplier;
        cfg.vabtCoeff            = vabtCoeff;
        cfg.ouMinTheta           = ouMinTheta;
    }
};

// ============================================================
//  ErrorMemory — Mémoire des erreurs mathématiques
// ============================================================
class ErrorMemory {
public:
    static constexpr int MAX_ERRORS     = 500;   // buffer circulaire
    static constexpr int MIN_FOR_CALIB  = 10;    // minimum pour calibration
    static constexpr int NUM_TYPES      = (int)ErrorType::COUNT;

    explicit ErrorMemory(int maxErrors = MAX_ERRORS,
                         const std::string& csvPath = "data/error_log.csv")
        : maxErrors_(maxErrors)
        , csvPath_(csvPath)
        , trendEwma_(0.15)
        , cycleCount_(0)
    {
        std::filesystem::create_directories("data");
        initCsvHeader();
    }

    // ─── Enregistrement d'une erreur ─────────────────────────
    void record(TradeError err) noexcept {
        std::lock_guard lk(mu_);
        ++cycleCount_;

        // Calcul du vecteur de correction Adam avant insertion
        computeCorrection(err);

        errors_.push_back(std::move(err));
        if ((int)errors_.size() > maxErrors_)
            errors_.pop_front();

        updateStats();
        appendCsv(errors_.back());
    }

    // ─── Construction d'une erreur depuis un cycle fermé ──────
    //  Appelé par TOP4EA_Engine à chaque fermeture de basket
    TradeError buildCycleError(
        double atrPoints,
        double atrMult,
        double gapUsed,
        double gapOptimal,
        double rho1,
        int    regime,
        double netDelta,
        double hedgePressure,
        int    recovDepth,
        double lotUsed,
        double pnlResult,
        double targetExpected,
        double drawdownPct
    ) const noexcept;

    // ─── Construction d'une erreur de cascade ────────────────
    TradeError buildCascadeError(
        double atrPoints,
        double atrMult,
        double gapUsed,
        int    recovDepth,
        double drawdownPct
    ) const noexcept;

    // ─── Statistiques par type ───────────────────────────────
    [[nodiscard]] ErrorStats stats(ErrorType t) const noexcept {
        std::lock_guard lk(mu_);
        return stats_[(int)t];
    }

    [[nodiscard]] int totalCount()  const noexcept {
        std::lock_guard lk(mu_); return (int)errors_.size();
    }
    [[nodiscard]] int cycleCount()  const noexcept { return cycleCount_; }
    [[nodiscard]] bool readyForCalibration() const noexcept {
        return cycleCount_ >= MIN_FOR_CALIB;
    }

    // ─── Vecteur gradient moyen pour calibration ─────────────
    //  Retourne la correction moyenne pondérée par sévérité
    //  sur les N dernières erreurs
    struct GradientVector {
        double deltaAtrMult      = 0.0;
        double deltaKellyFrac    = 0.0;
        double deltaGapExpansion = 0.0;
        double deltaTrailAsym    = 0.0;
        double deltaVelocityTrig = 0.0;
        int    nSamples          = 0;
    };
    [[nodiscard]] GradientVector gradientVector(int lastN = 20) const noexcept;

    // ─── Rapport textuel ─────────────────────────────────────
    [[nodiscard]] std::string report() const;

    // ─── Export CSV pour AutoML Python ───────────────────────
    void exportForAutoML(const std::string& path = "data/automl_training.csv") const;

private:
    void computeCorrection(TradeError& err) const noexcept;
    void updateStats() noexcept;
    void initCsvHeader() const;
    void appendCsv(const TradeError& e) const;

    int                        maxErrors_;
    std::string                csvPath_;
    std::deque<TradeError>     errors_;
    std::array<ErrorStats, NUM_TYPES> stats_ = {};
    math::EWMA                 trendEwma_;
    int                        cycleCount_;
    mutable std::mutex         mu_;
};

} // namespace top4ea
