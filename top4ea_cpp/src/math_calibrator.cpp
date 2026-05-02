// ============================================================
//  TOP4EA — math_calibrator.cpp
//  Auto-calibration par Adam + sortie PyTorch AutoML
// ============================================================
#include "../include/math_calibrator.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <fstream>
#include <algorithm>
#include <filesystem>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  Construction
// ────────────────────────────────────────────────────────────
MathCalibrator::MathCalibrator(const Config& cfg,
                                const std::string& automlJsonPath)
    : automlJsonPath_(automlJsonPath)
    , exportCsvPath_("data/automl_training.csv")
{
    std::filesystem::create_directories("data");
    std::filesystem::create_directories("automl/output");

    // Initialiser les params depuis la config
    params_.atrMultiplier       = cfg.atrMultiplier;
    params_.kellyMaxFraction    = cfg.kellyMaxFraction;
    params_.gapExpansionFactor  = cfg.gapExpansionFactor;
    params_.trailAsymmetryFactor= cfg.trailAsymmetryFactor;
    params_.velocityTrigger     = cfg.velocityTrigger;
    params_.cppiMultiplier      = cfg.cppiMultiplier;
    params_.vabtCoeff           = cfg.vabtCoeff;
    params_.ouMinTheta          = cfg.ouMinTheta;

    // Tenter de charger un état sauvegardé
    loadState();

    // Migration schéma CSV AutoML si nécessaire (colonnes Dual Trail absentes)
    migrateAutoMLSchema();
}

// ────────────────────────────────────────────────────────────
//  Pas Adam sur un paramètre scalaire
// ────────────────────────────────────────────────────────────
double MathCalibrator::adamStep(double param,
                                 double gradient,
                                 double& m,
                                 double& v,
                                 int    t) const noexcept
{
    if (t == 0) return param;

    // Correction de biais Adam
    m = ADAM_B1 * m + (1.0 - ADAM_B1) * gradient;
    v = ADAM_B2 * v + (1.0 - ADAM_B2) * gradient * gradient;

    double mHat = m / (1.0 - std::pow(ADAM_B1, t));
    double vHat = v / (1.0 - std::pow(ADAM_B2, t));

    return param - ADAM_LR * mHat / (std::sqrt(vHat) + ADAM_EPS);
}

// ────────────────────────────────────────────────────────────
//  Calibration depuis la mémoire d'erreurs (un pas Adam)
// ────────────────────────────────────────────────────────────
CalibrationResult MathCalibrator::calibrate(const ErrorMemory& memory) noexcept {
    if (!memory.readyForCalibration()) {
        CalibrationResult r;
        r.params  = params_;
        r.summary = "Données insuffisantes pour calibration (< "
                  + std::to_string(ErrorMemory::MIN_FOR_CALIB) + " cycles)";
        return r;
    }

    std::lock_guard lk(mu_);
    CalibrableParams before = params_;
    CalibrationResult result;

    // Récupérer le gradient moyen (20 dernières erreurs)
    auto gv = memory.gradientVector(20);
    if (gv.nSamples == 0) {
        result.params  = params_;
        result.summary = "Aucun gradient disponible";
        return result;
    }

    ++params_.t_adam;
    ++calibStep_;
    ++totalCalibrations_;

    // ── Mise à jour Adam pour chaque paramètre ────────────────

    // θ₁ — atrMultiplier
    params_.atrMultiplier = adamStep(
        params_.atrMultiplier, gv.deltaAtrMult,
        params_.m_atr, params_.v_atr, params_.t_adam);
    params_.atrMultiplier = ParamBounds::clamp(
        params_.atrMultiplier, ParamBounds::atrMultiplier);

    // θ₂ — kellyMaxFraction
    params_.kellyMaxFraction = adamStep(
        params_.kellyMaxFraction, gv.deltaKellyFrac,
        params_.m_kelly, params_.v_kelly, params_.t_adam);
    params_.kellyMaxFraction = ParamBounds::clamp(
        params_.kellyMaxFraction, ParamBounds::kellyMaxFraction);

    // θ₃ — gapExpansionFactor
    params_.gapExpansionFactor = adamStep(
        params_.gapExpansionFactor, gv.deltaGapExpansion,
        params_.m_gap, params_.v_gap, params_.t_adam);
    params_.gapExpansionFactor = ParamBounds::clamp(
        params_.gapExpansionFactor, ParamBounds::gapExpansionFactor);

    // θ₄ — trailAsymmetryFactor
    params_.trailAsymmetryFactor = adamStep(
        params_.trailAsymmetryFactor, gv.deltaTrailAsym,
        params_.m_asym, params_.v_asym, params_.t_adam);
    params_.trailAsymmetryFactor = ParamBounds::clamp(
        params_.trailAsymmetryFactor, ParamBounds::trailAsymmetry);

    // θ₅ — velocityTrigger
    params_.velocityTrigger = adamStep(
        params_.velocityTrigger, gv.deltaVelocityTrig,
        params_.m_vel, params_.v_vel, params_.t_adam);
    params_.velocityTrigger = ParamBounds::clamp(
        params_.velocityTrigger, ParamBounds::velocityTrigger);

    validateParams(params_);

    // ── Tenter chargement AutoML (si fichier récent disponible) ──
    if (loadFromAutoML()) {
        result.autoMLApplied = true;
        ++autoMLLoads_;
    }

    result.params           = params_;
    result.gradientSteps    = gv.nSamples;
    result.improvementScore = computeImprovementScore(before, params_);
    avgImprovement_ = avgImprovement_ * 0.9 + result.improvementScore * 0.1;

    // ── Sauvegarde état ───────────────────────────────────────
    saveState();

    // ── Résumé ────────────────────────────────────────────────
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4);
    oss << "CALIBRATION #" << totalCalibrations_
        << " [Adam t=" << params_.t_adam
        << " N=" << gv.nSamples << "]\n";
    oss << "  atrMult: " << before.atrMultiplier
        << " → " << params_.atrMultiplier << "\n";
    oss << "  kelly:   " << before.kellyMaxFraction
        << " → " << params_.kellyMaxFraction << "\n";
    oss << "  gapExp:  " << before.gapExpansionFactor
        << " → " << params_.gapExpansionFactor << "\n";
    oss << "  asym:    " << before.trailAsymmetryFactor
        << " → " << params_.trailAsymmetryFactor << "\n";
    oss << "  velTrig: " << before.velocityTrigger
        << " → " << params_.velocityTrigger << "\n";
    oss << "  amélioration estimée: " << (result.improvementScore * 100.0) << "%";
    if (result.autoMLApplied) oss << " | AutoML appliqué ✓";
    result.summary = oss.str();

    return result;
}

// ────────────────────────────────────────────────────────────
//  Chargement des paramètres PyTorch (JSON)
// ────────────────────────────────────────────────────────────
bool MathCalibrator::loadFromAutoML() noexcept {
    if (!std::filesystem::exists(automlJsonPath_)) return false;

    // Convertir l'horodatage du fichier vers system_clock (C++20)
    auto now     = SysClock::now();
    auto fwt     = std::filesystem::last_write_time(automlJsonPath_);
    auto fileTime = std::chrono::clock_cast<SysClock>(fwt);

    // Ne pas relire si pas assez de temps écoulé depuis la dernière lecture
    auto sinceLastRead = std::chrono::duration_cast<std::chrono::seconds>(
        now - lastAutoMLRead_).count();
    if (sinceLastRead < AUTOML_RELOAD_SEC) return false;

    // Ne recharger que si le fichier a été modifié depuis la dernière lecture
    if (fileTime <= lastAutoMLRead_) return false;

    auto opt = parseAutoMLJson(automlJsonPath_);
    if (!opt.has_value()) return false;

    // Fusion : les paramètres AutoML ont un poids de 0.30 (blending)
    // Le reste (0.70) reste la calibration Adam
    const double W_AUTOML = 0.30;
    const double W_ADAM   = 0.70;

    auto& ml = opt.value();
    params_.atrMultiplier       = W_ADAM * params_.atrMultiplier
                                + W_AUTOML * ml.atrMultiplier;
    params_.kellyMaxFraction    = W_ADAM * params_.kellyMaxFraction
                                + W_AUTOML * ml.kellyMaxFraction;
    params_.gapExpansionFactor  = W_ADAM * params_.gapExpansionFactor
                                + W_AUTOML * ml.gapExpansionFactor;
    params_.trailAsymmetryFactor= W_ADAM * params_.trailAsymmetryFactor
                                + W_AUTOML * ml.trailAsymmetryFactor;
    params_.velocityTrigger     = W_ADAM * params_.velocityTrigger
                                + W_AUTOML * ml.velocityTrigger;
    params_.cppiMultiplier      = W_ADAM * params_.cppiMultiplier
                                + W_AUTOML * ml.cppiMultiplier;
    params_.vabtCoeff           = W_ADAM * params_.vabtCoeff
                                + W_AUTOML * ml.vabtCoeff;

    validateParams(params_);
    lastAutoMLRead_ = now;
    return true;
}

// ────────────────────────────────────────────────────────────
//  Parsing JSON AutoML (simple, sans dépendance externe)
//  Format attendu : {"atr_mult": 0.55, "kelly_frac": 0.22, ...}
// ────────────────────────────────────────────────────────────
std::optional<CalibrableParams> MathCalibrator::parseAutoMLJson(
    const std::string& path) const noexcept
{
    std::ifstream f(path);
    if (!f) return std::nullopt;

    std::string content((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

    CalibrableParams p;
    bool anyFound = false;

    auto extractDouble = [&](const std::string& key, double& val) {
        auto pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) return;
        auto colon = content.find(':', pos);
        if (colon == std::string::npos) return;
        try {
            size_t end;
            val = std::stod(content.substr(colon + 1), &end);
            anyFound = true;
        } catch (...) {}
    };

    extractDouble("atr_mult",      p.atrMultiplier);
    extractDouble("kelly_frac",    p.kellyMaxFraction);
    extractDouble("gap_expansion", p.gapExpansionFactor);
    extractDouble("trail_asym",    p.trailAsymmetryFactor);
    extractDouble("vel_trigger",   p.velocityTrigger);
    extractDouble("cppi_mult",     p.cppiMultiplier);
    extractDouble("vabt_coeff",    p.vabtCoeff);
    extractDouble("ou_theta",      p.ouMinTheta);

    if (!anyFound) return std::nullopt;

    // Validation des bornes
    p.atrMultiplier       = std::clamp(p.atrMultiplier,       0.20, 2.00);
    p.kellyMaxFraction    = std::clamp(p.kellyMaxFraction,    0.05, 0.50);
    p.gapExpansionFactor  = std::clamp(p.gapExpansionFactor,  0.01, 0.30);
    p.trailAsymmetryFactor= std::clamp(p.trailAsymmetryFactor,0.05, 0.40);
    p.velocityTrigger     = std::clamp(p.velocityTrigger,     0.60, 0.98);
    p.cppiMultiplier      = std::clamp(p.cppiMultiplier,      1.50, 6.00);
    p.vabtCoeff           = std::clamp(p.vabtCoeff,           0.05, 0.50);
    p.ouMinTheta          = std::clamp(p.ouMinTheta,          0.01, 0.30);

    return p;
}

// ────────────────────────────────────────────────────────────
//  Migration schéma CSV AutoML (colonnes Dual Trail absentes)
//  Réécrit le fichier existant en ajoutant 8 colonnes zeroed
//  avant label_good si le header ne contient pas trail_mode_used
// ────────────────────────────────────────────────────────────
void MathCalibrator::migrateAutoMLSchema() noexcept {
    if (!std::filesystem::exists(exportCsvPath_)) return;

    // Lire le header existant
    std::ifstream fin(exportCsvPath_);
    if (!fin) return;
    std::string header;
    if (!std::getline(fin, header)) return;

    // Si le header contient déjà trail_mode_used, aucune migration nécessaire
    if (header.find("trail_mode_used") != std::string::npos) return;

    // Lire toutes les lignes de données restantes
    std::vector<std::string> dataLines;
    std::string line;
    while (std::getline(fin, line)) {
        if (!line.empty()) dataLines.push_back(std::move(line));
    }
    fin.close();

    // Nouveau header (8 colonnes Dual Trail insérées avant label_good)
    static constexpr std::string_view NEW_HEADER =
        "timestamp,atr_pts,rho1,regime,net_delta,hedge_pressure,"
        "recov_depth,lot_used,pnl_result,drawdown_pct,"
        "ofi_score,imbalance_score,"
        "atr_mult_used,kelly_frac_used,gap_exp_used,trail_asym_used,"
        "trail_mode_used,v_lock_triggered,v_lock_price,ss_profit_at_lock_pts,"
        "trail_mode_buy_used,ss_lock_triggered,ss_lock_price,bs_profit_at_lock_pts,"
        "label_good";

    // Écrire dans un fichier temporaire, puis renommer (atomique)
    std::string tmpPath = exportCsvPath_ + ".migrating";
    std::ofstream fout(tmpPath);
    if (!fout) return;

    fout << NEW_HEADER << "\n";

    // Pour chaque ancienne ligne : insérer 8 zéros avant le dernier champ
    for (auto& dl : dataLines) {
        // Trouver la dernière virgule → tout ce qui est avant + 8 zéros + label
        auto lastComma = dl.rfind(',');
        if (lastComma == std::string::npos) {
            fout << dl << "\n";   // ligne malformée : conserver telle quelle
            continue;
        }
        std::string prefix = dl.substr(0, lastComma);
        std::string label  = dl.substr(lastComma + 1);
        fout << prefix
             << ",0,0,0.000000,0.0"   // trail_mode_used, v_lock_triggered, v_lock_price, ss_profit_at_lock_pts
             << ",0,0,0.000000,0.0"   // trail_mode_buy_used, ss_lock_triggered, ss_lock_price, bs_profit_at_lock_pts
             << "," << label << "\n";
    }
    fout.close();

    // Remplacer l'ancien fichier (rename atomique sur POSIX)
    std::error_code ec;
    std::filesystem::rename(tmpPath, exportCsvPath_, ec);
    // En cas d'échec (cross-device), copier puis supprimer
    if (ec) {
        std::filesystem::copy_file(tmpPath, exportCsvPath_,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) std::filesystem::remove(tmpPath, ec);
    }
}

// ────────────────────────────────────────────────────────────
//  Export d'un cycle pour l'entraînement AutoML
// ────────────────────────────────────────────────────────────
void MathCalibrator::exportCycleToAutoML(
    double atrPoints, double rho1, int regime,
    double netDelta, double hedgePressure, int recovDepth,
    double lotUsed, double pnlResult, double drawdownPct,
    double ofiScore, double imbalanceScore,
    double atrMultUsed, double kellyFracUsed,
    double gapExpUsed, double trailAsymUsed,
    int trailModeUsed, int vLockTriggered, double vLockPrice, double ssProfitAtLockPts,
    int trailModeBuyUsed, int ssLockTriggered, double ssLockPrice, double bsProfitAtLockPts
) const noexcept {
    // Créer le répertoire data/ si absent
    std::filesystem::create_directories(std::filesystem::path(exportCsvPath_).parent_path());

    // Header si fichier absent
    bool writeHeader = !std::filesystem::exists(exportCsvPath_);
    std::ofstream f(exportCsvPath_, std::ios::app);
    if (!f) return;

    if (writeHeader) {
        f << "timestamp,atr_pts,rho1,regime,net_delta,hedge_pressure,"
          << "recov_depth,lot_used,pnl_result,drawdown_pct,"
          << "ofi_score,imbalance_score,"
          << "atr_mult_used,kelly_frac_used,gap_exp_used,trail_asym_used,"
          << "trail_mode_used,v_lock_triggered,v_lock_price,ss_profit_at_lock_pts,"
          << "trail_mode_buy_used,ss_lock_triggered,ss_lock_price,bs_profit_at_lock_pts,"
          << "label_good\n";
    }

    // Label : 1 si cycle profitable (> 80% du target), 0 sinon
    // On ne connaît pas le target exact ici, mais pnl > 0 est un proxy
    int label = (pnlResult > 0.0 && drawdownPct < 10.0) ? 1 : 0;

    auto now = std::time(nullptr);
    f << std::fixed << std::setprecision(6);
    f << now << ","
      << atrPoints << "," << rho1 << "," << regime << ","
      << netDelta << "," << hedgePressure << "," << recovDepth << ","
      << lotUsed << "," << pnlResult << "," << drawdownPct << ","
      << ofiScore << "," << imbalanceScore << ","
      << atrMultUsed << "," << kellyFracUsed << ","
      << gapExpUsed << "," << trailAsymUsed << ","
      << trailModeUsed << "," << vLockTriggered << ","
      << std::setprecision(5) << vLockPrice << ","
      << std::setprecision(1) << ssProfitAtLockPts << ","
      << trailModeBuyUsed << "," << ssLockTriggered << ","
      << std::setprecision(5) << ssLockPrice << ","
      << std::setprecision(1) << bsProfitAtLockPts << ","
      << label << "\n";
}

// ────────────────────────────────────────────────────────────
//  Score d'amélioration estimé
// ────────────────────────────────────────────────────────────
double MathCalibrator::computeImprovementScore(
    const CalibrableParams& before,
    const CalibrableParams& after) const noexcept
{
    // Mesure la variation relative normalisée des paramètres
    double total = 0.0;
    auto relChange = [](double a, double b) {
        return std::abs(b - a) / (std::abs(a) + 1e-9);
    };

    total += relChange(before.atrMultiplier,       after.atrMultiplier);
    total += relChange(before.kellyMaxFraction,    after.kellyMaxFraction);
    total += relChange(before.gapExpansionFactor,  after.gapExpansionFactor);
    total += relChange(before.trailAsymmetryFactor,after.trailAsymmetryFactor);
    total += relChange(before.velocityTrigger,     after.velocityTrigger);

    return std::min(1.0, total / 5.0 * 10.0);  // normalisé [0,1]
}

// ────────────────────────────────────────────────────────────
//  Validation et correction des bornes
// ────────────────────────────────────────────────────────────
void MathCalibrator::validateParams(CalibrableParams& p) const noexcept {
    p.atrMultiplier       = std::clamp(p.atrMultiplier,        0.20, 2.00);
    p.kellyMaxFraction    = std::clamp(p.kellyMaxFraction,     0.05, 0.50);
    p.gapExpansionFactor  = std::clamp(p.gapExpansionFactor,   0.01, 0.30);
    p.trailAsymmetryFactor= std::clamp(p.trailAsymmetryFactor, 0.05, 0.40);
    p.velocityTrigger     = std::clamp(p.velocityTrigger,      0.60, 0.98);
    p.cppiMultiplier      = std::clamp(p.cppiMultiplier,       1.50, 6.00);
    p.vabtCoeff           = std::clamp(p.vabtCoeff,            0.05, 0.50);
    p.ouMinTheta          = std::clamp(p.ouMinTheta,           0.01, 0.30);
}

// ────────────────────────────────────────────────────────────
//  Rapport de calibration
// ────────────────────────────────────────────────────────────
std::string MathCalibrator::report() const {
    std::lock_guard lk(mu_);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4);
    oss << "=== AUTO-CALIBRATION ÉTAT ===\n";
    oss << "Calibrations totales : " << totalCalibrations_ << "\n";
    oss << "AutoML chargements   : " << autoMLLoads_ << "\n";
    oss << "Amélioration moyenne : " << (avgImprovement_ * 100.0) << "%\n\n";
    oss << "PARAMÈTRES COURANTS :\n";
    oss << "  atrMultiplier      = " << params_.atrMultiplier
        << "  [0.20, 2.00]\n";
    oss << "  kellyMaxFraction   = " << params_.kellyMaxFraction
        << "  [0.05, 0.50]\n";
    oss << "  gapExpansionFactor = " << params_.gapExpansionFactor
        << "  [0.01, 0.30]\n";
    oss << "  trailAsymmetry     = " << params_.trailAsymmetryFactor
        << "  [0.05, 0.40]\n";
    oss << "  velocityTrigger    = " << params_.velocityTrigger
        << "  [0.60, 0.98]\n";
    oss << "  cppiMultiplier     = " << params_.cppiMultiplier
        << "  [1.50, 6.00]\n";
    oss << "  vabtCoeff          = " << params_.vabtCoeff
        << "  [0.05, 0.50]\n";
    oss << "  ouMinTheta         = " << params_.ouMinTheta
        << "  [0.01, 0.30]\n";
    oss << "  Adam t             = " << params_.t_adam << "\n";
    return oss.str();
}

// ────────────────────────────────────────────────────────────
//  Persistance JSON (sans dépendance externe)
// ────────────────────────────────────────────────────────────
void MathCalibrator::saveState(const std::string& path) const noexcept {
    std::lock_guard lk(mu_);
    std::ofstream f(path);
    if (!f) return;

    f << std::fixed << std::setprecision(8);
    f << "{\n";
    f << "  \"atr_mult\": "      << params_.atrMultiplier       << ",\n";
    f << "  \"kelly_frac\": "    << params_.kellyMaxFraction     << ",\n";
    f << "  \"gap_expansion\": " << params_.gapExpansionFactor   << ",\n";
    f << "  \"trail_asym\": "    << params_.trailAsymmetryFactor << ",\n";
    f << "  \"vel_trigger\": "   << params_.velocityTrigger      << ",\n";
    f << "  \"cppi_mult\": "     << params_.cppiMultiplier       << ",\n";
    f << "  \"vabt_coeff\": "    << params_.vabtCoeff            << ",\n";
    f << "  \"ou_theta\": "      << params_.ouMinTheta           << ",\n";
    f << "  \"adam_t\": "        << params_.t_adam               << ",\n";
    f << "  \"m_atr\": "         << params_.m_atr                << ",\n";
    f << "  \"v_atr\": "         << params_.v_atr                << ",\n";
    f << "  \"m_kelly\": "       << params_.m_kelly              << ",\n";
    f << "  \"v_kelly\": "       << params_.v_kelly              << ",\n";
    f << "  \"m_gap\": "         << params_.m_gap                << ",\n";
    f << "  \"v_gap\": "         << params_.v_gap                << ",\n";
    f << "  \"m_asym\": "        << params_.m_asym               << ",\n";
    f << "  \"v_asym\": "        << params_.v_asym               << ",\n";
    f << "  \"m_vel\": "         << params_.m_vel                << ",\n";
    f << "  \"v_vel\": "         << params_.v_vel                << "\n";
    f << "}\n";
}

// ────────────────────────────────────────────────────────────
//  Parsing JSON état complet (paramètres + moments Adam)
//  Utilisé pour charger l'état sauvegardé par saveState()
// ────────────────────────────────────────────────────────────
bool MathCalibrator::parseStateJson(const std::string& path,
                                     CalibrableParams& out) const noexcept {
    std::ifstream f(path);
    if (!f) return false;

    std::string content((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

    bool anyFound = false;

    auto extractInt = [&](const std::string& key, int& val) {
        auto pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) return;
        auto colon = content.find(':', pos);
        if (colon == std::string::npos) return;
        try {
            size_t end;
            val = std::stoi(content.substr(colon + 1), &end);
            anyFound = true;
        } catch (...) {}
    };
    auto extractDouble = [&](const std::string& key, double& val) {
        auto pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) return;
        auto colon = content.find(':', pos);
        if (colon == std::string::npos) return;
        try {
            size_t end;
            val = std::stod(content.substr(colon + 1), &end);
            anyFound = true;
        } catch (...) {}
    };

    // Paramètres de trading
    extractDouble("atr_mult",      out.atrMultiplier);
    extractDouble("kelly_frac",    out.kellyMaxFraction);
    extractDouble("gap_expansion", out.gapExpansionFactor);
    extractDouble("trail_asym",    out.trailAsymmetryFactor);
    extractDouble("vel_trigger",   out.velocityTrigger);
    extractDouble("cppi_mult",     out.cppiMultiplier);
    extractDouble("vabt_coeff",    out.vabtCoeff);
    extractDouble("ou_theta",      out.ouMinTheta);

    // Moments Adam (état de l'optimiseur)
    extractInt   ("adam_t",  out.t_adam);
    extractDouble("m_atr",   out.m_atr);
    extractDouble("v_atr",   out.v_atr);
    extractDouble("m_kelly", out.m_kelly);
    extractDouble("v_kelly", out.v_kelly);
    extractDouble("m_gap",   out.m_gap);
    extractDouble("v_gap",   out.v_gap);
    extractDouble("m_asym",  out.m_asym);
    extractDouble("v_asym",  out.v_asym);
    extractDouble("m_vel",   out.m_vel);
    extractDouble("v_vel",   out.v_vel);

    return anyFound;
}

bool MathCalibrator::loadState(const std::string& path) noexcept {
    if (!std::filesystem::exists(path)) return false;

    CalibrableParams loaded = params_;  // partir des valeurs actuelles
    if (!parseStateJson(path, loaded)) return false;

    std::lock_guard lk(mu_);
    params_ = loaded;
    validateParams(params_);
    return true;
}

} // namespace top4ea
