#pragma once
// ============================================================
//  TOP4EA — math_calibrator.hpp
//  Auto-calibration mathématique des paramètres de trading
//
//  PHILOSOPHIE (Hedge Institutionnel Auto-apprenant) :
//  ─────────────────────────────────────────────────────────
//  Les paramètres mathématiques du système ne sont PAS fixes.
//  Ils sont recalibrés en continu sur la base de :
//
//  [A] Erreurs historiques (ErrorMemory)
//      → Gradient descent sur le vecteur θ des paramètres
//      → Algorithme Adam (Kingma & Ba, 2014) sans bibliothèque
//
//  [B] Performance rolling (RollingKelly)
//      → Ajustement du baseLot et kellyFraction
//      → Basé sur les N derniers cycles fermés
//
//  [C] Sortie AutoML PyTorch (fichier JSON)
//      → Paramètres optimisés par le réseau de neurones
//      → Lus toutes les 5 minutes si le fichier est plus récent
//
//  [D] Contraintes institutionnelles
//      → Bornes dures sur chaque paramètre
//      → Validation de cohérence mathématique
//
//  PARAMÈTRES CALIBRABLES θ :
//  ─────────────────────────────────────────────────────────
//  θ₁ = atrMultiplier      ∈ [0.20, 2.00]
//  θ₂ = kellyMaxFraction   ∈ [0.05, 0.50]
//  θ₃ = gapExpansionFactor ∈ [0.01, 0.30]
//  θ₄ = trailAsymmetryFac  ∈ [0.05, 0.40]
//  θ₅ = velocityTrigger    ∈ [0.60, 0.98]
//  θ₆ = cppiMultiplier     ∈ [1.50, 6.00]
//  θ₇ = vabtCoeff          ∈ [0.05, 0.50]
//  θ₈ = ouMinTheta         ∈ [0.01, 0.30]
//
//  MÉTHODE D'OPTIMISATION :
//  ─────────────────────────────────────────────────────────
//  Adam (Adaptive Moment Estimation) :
//    m_t = β₁ × m_{t-1} + (1-β₁) × g_t     (1er moment)
//    v_t = β₂ × v_{t-1} + (1-β₂) × g_t²    (2ème moment)
//    m̂_t = m_t / (1 - β₁^t)                 (correction biais)
//    v̂_t = v_t / (1 - β₂^t)                 (correction biais)
//    θ_t = θ_{t-1} - η × m̂_t / (√v̂_t + ε) (mise à jour)
//
//  Paramètres : η=0.01, β₁=0.90, β₂=0.999, ε=1e-8
// ============================================================
#include "types.hpp"
#include "error_memory.hpp"
#include <mutex>
#include <string>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <optional>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  Bornes institutionnelles des paramètres
// ────────────────────────────────────────────────────────────
struct ParamBounds {
    struct Bound { double min, max; };

    static constexpr Bound atrMultiplier      { 0.20, 2.00 };
    static constexpr Bound kellyMaxFraction   { 0.05, 0.50 };
    static constexpr Bound gapExpansionFactor { 0.01, 0.30 };
    static constexpr Bound trailAsymmetry     { 0.05, 0.40 };
    static constexpr Bound velocityTrigger    { 0.60, 0.98 };
    static constexpr Bound cppiMultiplier     { 1.50, 6.00 };
    static constexpr Bound vabtCoeff          { 0.05, 0.50 };
    static constexpr Bound ouMinTheta         { 0.01, 0.30 };

    static double clamp(double v, const Bound& b) noexcept {
        return std::clamp(v, b.min, b.max);
    }
};

// ────────────────────────────────────────────────────────────
//  Résultat d'un cycle de calibration
// ────────────────────────────────────────────────────────────
struct CalibrationResult {
    CalibrableParams params;
    double           improvementScore = 0.0;  // amélioration estimée [0,1]
    int              gradientSteps    = 0;    // nombre de pas Adam effectués
    bool             autoMLApplied    = false; // sortie PyTorch appliquée
    std::string      summary;
};

// ============================================================
//  MathCalibrator — Auto-calibration des paramètres
// ============================================================
class MathCalibrator {
public:
    // Hyperparamètres Adam
    static constexpr double ADAM_LR   = 0.01;   // learning rate
    static constexpr double ADAM_B1   = 0.90;   // β₁ (1er moment)
    static constexpr double ADAM_B2   = 0.999;  // β₂ (2ème moment)
    static constexpr double ADAM_EPS  = 1e-8;   // ε stabilisation

    // Fréquence de lecture du JSON AutoML (en secondes)
    static constexpr int AUTOML_RELOAD_SEC = 300; // 5 minutes

    explicit MathCalibrator(const Config& cfg,
                            const std::string& automlJsonPath = "automl/output/params.json");

    // ─── Calibration depuis la mémoire d'erreurs ─────────────
    //  Effectue un pas de gradient Adam sur le vecteur θ
    //  Retourne les paramètres mis à jour
    CalibrationResult calibrate(const ErrorMemory& memory) noexcept;

    // ─── Calibration depuis la sortie AutoML ─────────────────
    //  Lit le fichier JSON PyTorch et applique les paramètres
    //  Si le fichier n'existe pas ou est trop ancien → skip
    bool loadFromAutoML() noexcept;

    // ─── Export d'un cycle pour entraînement AutoML ──────────
    //  Écrit les données du cycle dans le CSV de training
    void exportCycleToAutoML(
        double atrPoints,
        double rho1,
        int    regime,
        double netDelta,
        double hedgePressure,
        int    recovDepth,
        double lotUsed,
        double pnlResult,
        double drawdownPct,
        double ofiScore,
        double imbalanceScore,
        // Paramètres utilisés dans ce cycle
        double atrMultUsed,
        double kellyFracUsed,
        double gapExpUsed,
        double trailAsymUsed,
        // Métriques Dual Trail (SELL-side BS AGGRESSIVE)
        int    trailModeUsed,      // 0=NATURAL, 1=AGGRESSIVE
        int    vLockTriggered,     // 1 si verrou BS actif au close du cycle
        double vLockPrice,         // niveau verrou BS (0.0 si inactif)
        double ssProfitAtLockPts,  // profit Sell flottant en pts au moment du lock
        // Métriques Dual Trail (BUY-side SS AGGRESSIVE miroir)
        int    trailModeBuyUsed,   // 0=NATURAL, 1=AGGRESSIVE
        int    ssLockTriggered,    // 1 si verrou SS BUY actif au close du cycle
        double ssLockPrice,        // niveau verrou SS BUY (0.0 si inactif)
        double bsProfitAtLockPts   // profit Buy flottant en pts au moment du lock
    ) const noexcept;

    // ─── Application à la config ──────────────────────────────
    void applyToConfig(Config& cfg) const noexcept {
        std::lock_guard lk(mu_);
        params_.applyToConfig(cfg);
    }

    // ─── Accès aux paramètres courants ───────────────────────
    [[nodiscard]] CalibrableParams currentParams() const noexcept {
        std::lock_guard lk(mu_);
        return params_;
    }

    // ─── Rapport de calibration ───────────────────────────────
    [[nodiscard]] std::string report() const;

    // ─── Persistance ─────────────────────────────────────────
    void saveState(const std::string& path = "data/calibration_state.json") const noexcept;
    bool loadState(const std::string& path = "data/calibration_state.json") noexcept;

private:
    // ─── Pas Adam sur un paramètre ────────────────────────────
    double adamStep(double param,
                    double gradient,
                    double& m,
                    double& v,
                    int    t) const noexcept;

    // ─── Lecture JSON AutoML ──────────────────────────────────
    std::optional<CalibrableParams> parseAutoMLJson(const std::string& path) const noexcept;

    // ─── Lecture JSON état sauvegardé ─────────────────────────
    bool parseStateJson(const std::string& path, CalibrableParams& out) const noexcept;

    // ─── Score d'amélioration ─────────────────────────────────
    double computeImprovementScore(const CalibrableParams& before,
                                   const CalibrableParams& after) const noexcept;

    // ─── Vérification de cohérence ────────────────────────────
    void validateParams(CalibrableParams& p) const noexcept;

    CalibrableParams   params_;
    std::string        automlJsonPath_;
    std::string        exportCsvPath_;
    int                calibStep_    = 0;  // compteur de pas globaux

    mutable std::mutex mu_;

    // Dernière lecture AutoML
    using SysClock = std::chrono::system_clock;
    mutable SysClock::time_point lastAutoMLRead_ = SysClock::time_point{};

    // Statistiques
    int    totalCalibrations_ = 0;
    int    autoMLLoads_       = 0;
    double avgImprovement_    = 0.0;
};

} // namespace top4ea
