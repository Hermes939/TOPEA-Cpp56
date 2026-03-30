// ============================================================
//  TOP4EA — error_memory.cpp
//  Mémoire structurée des erreurs mathématiques
//  Auto-correction par gradient Adam
// ============================================================
#include "../include/error_memory.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <ctime>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  Construction des erreurs depuis des cycles réels
// ────────────────────────────────────────────────────────────
TradeError ErrorMemory::buildCycleError(
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
) const noexcept {
    TradeError e;
    e.atrPoints         = atrPoints;
    e.atrMultiplierUsed = atrMult;
    e.gapUsedPoints     = gapUsed;
    e.gapOptimalPoints  = gapOptimal;
    e.rho1              = rho1;
    e.regime            = regime;
    e.netDelta          = netDelta;
    e.hedgePressure     = hedgePressure;
    e.recovDepth        = recovDepth;
    e.lotUsed           = lotUsed;
    e.pnlResult         = pnlResult;
    e.targetExpected    = targetExpected;
    e.drawdownAtError   = drawdownPct;

    // ── Classification automatique de l'erreur ────────────────
    double gapError = (gapOptimal > 0.0)
                    ? std::abs(gapUsed - gapOptimal) / gapOptimal
                    : 0.0;

    if (pnlResult < 0.0 && recovDepth > 2) {
        // Cascade s'est produite avec récupération profonde → cascade évitable ?
        e.type     = ErrorType::CASCADE_AVOIDABLE;
        e.severity = std::min(1.0, std::abs(pnlResult) / (std::abs(targetExpected) + 1e-9));
    } else if (gapError > 0.25) {
        // Gap utilisé diffère de > 25% du gap optimal
        e.type     = ErrorType::GRID_MISPLACE;
        e.severity = std::min(1.0, gapError);
    } else if (hedgePressure > 0.40 && std::abs(netDelta) > 0.1) {
        // Déséquilibre delta important non traité
        e.type     = ErrorType::HEDGE_MISS;
        e.severity = std::min(1.0, hedgePressure);
    } else if (targetExpected > 0.0 && pnlResult > targetExpected * 1.10) {
        // Basket fermé trop tard
        e.type     = ErrorType::TARGET_OVERSHOOT;
        e.severity = std::min(1.0, (pnlResult / targetExpected - 1.0));
    } else if (targetExpected > 0.0 && pnlResult < targetExpected * 0.80 && pnlResult < 0) {
        // Target non atteint → retournement
        e.type     = ErrorType::TARGET_MISS;
        e.severity = std::min(1.0, std::abs(pnlResult) / (targetExpected + 1e-9));
    } else {
        // Lot sous-optimal si P&L très faible vs target
        e.type     = ErrorType::LOT_SUBOPTIMAL;
        e.severity = (targetExpected > 0.0)
                   ? std::max(0.0, 1.0 - pnlResult / targetExpected)
                   : 0.1;
    }

    return e;
}

TradeError ErrorMemory::buildCascadeError(
    double atrPoints,
    double atrMult,
    double gapUsed,
    int    recovDepth,
    double drawdownPct
) const noexcept {
    TradeError e;
    e.type              = ErrorType::CASCADE_AVOIDABLE;
    e.atrPoints         = atrPoints;
    e.atrMultiplierUsed = atrMult;
    e.gapUsedPoints     = gapUsed;
    e.recovDepth        = recovDepth;
    e.drawdownAtError   = drawdownPct;
    // Sévérité proportionnelle à la profondeur de recovery
    e.severity          = std::min(1.0, (double)recovDepth / 10.0);
    e.pnlResult         = -drawdownPct;
    return e;
}

// ────────────────────────────────────────────────────────────
//  Calcul du vecteur de correction (Adam implicite)
//  Chaque erreur contribue Δθ proportionnel à sa sévérité
// ────────────────────────────────────────────────────────────
void ErrorMemory::computeCorrection(TradeError& err) const noexcept {
    double s = err.severity;

    switch (err.type) {
        case ErrorType::GRID_MISPLACE: {
            // Gap trop petit → augmenter atrMultiplier
            double dir = (err.gapUsedPoints < err.gapOptimalPoints) ? +1.0 : -1.0;
            err.deltaAtrMult      = dir * s * 0.02;
            err.deltaGapExpansion = dir * s * 0.005;
            break;
        }
        case ErrorType::CASCADE_AVOIDABLE: {
            // Cascade évitable → augmenter gap d'expansion recovery
            err.deltaAtrMult      = +s * 0.015;
            err.deltaGapExpansion = +s * 0.010;
            err.deltaTrailAsym    = +s * 0.005;
            break;
        }
        case ErrorType::LOT_SUBOPTIMAL: {
            // Lot sous-optimal → ajuster Kelly fraction
            double dir = (err.lotUsed < err.lotOptimal) ? +1.0 : -1.0;
            err.deltaKellyFrac    = dir * s * 0.01;
            break;
        }
        case ErrorType::HEDGE_MISS: {
            // Hedge raté → augmenter asymétrie et trail
            err.deltaTrailAsym    = +s * 0.008;
            err.deltaAtrMult      = +s * 0.010;
            break;
        }
        case ErrorType::TARGET_OVERSHOOT: {
            // Fermé trop tard → déclencher plus tôt
            err.deltaVelocityTrig = -s * 0.015;  // baisser le seuil velocity
            break;
        }
        case ErrorType::TARGET_MISS: {
            // Target non atteint → velocityTrigger trop haut ou gap trop petit
            err.deltaVelocityTrig = +s * 0.010;
            err.deltaAtrMult      = -s * 0.008;
            break;
        }
        case ErrorType::IMBALANCE_MISS: {
            // Imbalance ignorée → élargir gap asymétrique
            err.deltaTrailAsym    = +s * 0.012;
            err.deltaGapExpansion = +s * 0.008;
            break;
        }
        case ErrorType::ASYMMETRY_ERROR: {
            // Asymétrie dans le mauvais sens → inverser
            err.deltaTrailAsym    = -s * 0.020;
            break;
        }
        default: break;
    }
}

// ────────────────────────────────────────────────────────────
//  Mise à jour des statistiques par type
// ────────────────────────────────────────────────────────────
void ErrorMemory::updateStats() noexcept {
    // Réinitialiser
    for (auto& st : stats_) {
        st.count    = 0;
        st.totalSev = 0.0;
    }

    for (const auto& e : errors_) {
        auto& st = stats_[(int)e.type];
        ++st.count;
        st.totalSev += e.severity;
    }

    for (auto& st : stats_) {
        if (st.count > 0) {
            st.avgSev   = st.totalSev / st.count;
            st.frequency= (cycleCount_ > 0)
                        ? (double)st.count / cycleCount_ * 100.0
                        : 0.0;
        }
    }
}

// ────────────────────────────────────────────────────────────
//  Gradient moyen pondéré par sévérité sur les N derniers
// ────────────────────────────────────────────────────────────
ErrorMemory::GradientVector ErrorMemory::gradientVector(int lastN) const noexcept {
    std::lock_guard lk(mu_);
    GradientVector gv;

    int start = std::max(0, (int)errors_.size() - lastN);
    double totalWeight = 0.0;

    for (int i = start; i < (int)errors_.size(); ++i) {
        const auto& e = errors_[i];
        double w = e.severity;
        gv.deltaAtrMult      += e.deltaAtrMult      * w;
        gv.deltaKellyFrac    += e.deltaKellyFrac    * w;
        gv.deltaGapExpansion += e.deltaGapExpansion * w;
        gv.deltaTrailAsym    += e.deltaTrailAsym    * w;
        gv.deltaVelocityTrig += e.deltaVelocityTrig * w;
        totalWeight          += w;
        ++gv.nSamples;
    }

    if (totalWeight > 1e-9) {
        gv.deltaAtrMult      /= totalWeight;
        gv.deltaKellyFrac    /= totalWeight;
        gv.deltaGapExpansion /= totalWeight;
        gv.deltaTrailAsym    /= totalWeight;
        gv.deltaVelocityTrig /= totalWeight;
    }

    return gv;
}

// ────────────────────────────────────────────────────────────
//  Rapport textuel
// ────────────────────────────────────────────────────────────
std::string ErrorMemory::report() const {
    std::lock_guard lk(mu_);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "=== MÉMOIRE DES ERREURS MATHÉMATIQUES ===\n";
    oss << "Cycles enregistrés: " << cycleCount_
        << " | Buffer erreurs: " << errors_.size() << "\n\n";

    for (int i = 0; i < NUM_TYPES; ++i) {
        const auto& st = stats_[i];
        if (st.count == 0) continue;
        oss << "[" << errorTypeName((ErrorType)i) << "]\n";
        oss << "  Count=" << st.count
            << " | Sévérité moy=" << st.avgSev
            << " | Fréquence=" << st.frequency << "%\n";
    }

    auto gv = gradientVector(20);
    oss << "\nGRADIENT (20 dernières erreurs):\n";
    oss << "  Δ atrMult=" << gv.deltaAtrMult
        << " | Δ kelly=" << gv.deltaKellyFrac
        << " | Δ gapExp=" << gv.deltaGapExpansion
        << " | Δ asym=" << gv.deltaTrailAsym
        << " | Δ velTrig=" << gv.deltaVelocityTrig << "\n";

    return oss.str();
}

// ────────────────────────────────────────────────────────────
//  Initialisation header CSV
// ────────────────────────────────────────────────────────────
void ErrorMemory::initCsvHeader() const {
    if (std::filesystem::exists(csvPath_)) return;
    std::ofstream f(csvPath_);
    if (!f) return;
    f << "timestamp,error_type,severity,atr_pts,atr_mult_used,"
      << "gap_used,gap_optimal,rho1,regime,net_delta,hedge_pressure,"
      << "recov_depth,lot_used,pnl_result,target_expected,drawdown_pct,"
      << "delta_atr,delta_kelly,delta_gap_exp,delta_trail_asym,delta_vel_trig\n";
}

// ────────────────────────────────────────────────────────────
//  Append ligne CSV
// ────────────────────────────────────────────────────────────
void ErrorMemory::appendCsv(const TradeError& e) const {
    std::ofstream f(csvPath_, std::ios::app);
    if (!f) return;

    auto tp = std::chrono::system_clock::to_time_t(e.ts);
    f << std::fixed << std::setprecision(6);
    f << tp << ","
      << (int)e.type << ","
      << e.severity << ","
      << e.atrPoints << ","
      << e.atrMultiplierUsed << ","
      << e.gapUsedPoints << ","
      << e.gapOptimalPoints << ","
      << e.rho1 << ","
      << e.regime << ","
      << e.netDelta << ","
      << e.hedgePressure << ","
      << e.recovDepth << ","
      << e.lotUsed << ","
      << e.pnlResult << ","
      << e.targetExpected << ","
      << e.drawdownAtError << ","
      << e.deltaAtrMult << ","
      << e.deltaKellyFrac << ","
      << e.deltaGapExpansion << ","
      << e.deltaTrailAsym << ","
      << e.deltaVelocityTrig << "\n";
}

// ────────────────────────────────────────────────────────────
//  Export pour AutoML Python
// ────────────────────────────────────────────────────────────
void ErrorMemory::exportForAutoML(const std::string& path) const {
    std::lock_guard lk(mu_);
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path());

    std::ofstream f(path);
    if (!f) return;

    f << "timestamp,error_type,severity,atr_pts,atr_mult,gap_used,"
      << "gap_optimal,rho1,regime,net_delta,hedge_pressure,recov_depth,"
      << "lot_used,pnl_result,target_expected,drawdown_pct\n";

    f << std::fixed << std::setprecision(6);
    for (const auto& e : errors_) {
        auto tp = std::chrono::system_clock::to_time_t(e.ts);
        f << tp << ","
          << (int)e.type << ","
          << e.severity << ","
          << e.atrPoints << ","
          << e.atrMultiplierUsed << ","
          << e.gapUsedPoints << ","
          << e.gapOptimalPoints << ","
          << e.rho1 << ","
          << e.regime << ","
          << e.netDelta << ","
          << e.hedgePressure << ","
          << e.recovDepth << ","
          << e.lotUsed << ","
          << e.pnlResult << ","
          << e.targetExpected << ","
          << e.drawdownAtError << "\n";
    }
}

} // namespace top4ea
