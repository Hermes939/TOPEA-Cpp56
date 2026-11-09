// ============================================================
//  TOP4EA — main.cpp
//  Point d'entrée C++20 — chargement config + démarrage
// ============================================================
#include "../include/top4ea_engine.hpp"
#include <iostream>
#include <csignal>
#include <atomic>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>

// ─── Signal handler pour arrêt propre ───────────────────────
static std::atomic<bool> g_running { true };

static void sigHandler(int) {
    g_running.store(false);
}

// ─── Helpers pour le loader de config ────────────────────────
static std::string getEnvOr(const char* key, const std::string& fallback = "") {
    const char* v = std::getenv(key);
    return (v && v[0] != '\0') ? std::string(v) : fallback;
}

// Applique une clé/valeur sur le Config (fichier ou env)
static void applyConfigKV(top4ea::Config& cfg,
                           const std::string& key,
                           const std::string& val,
                           bool logOverride = false) {
    if (logOverride)
        std::cout << "[CONFIG] " << key << "=" << val << " (env override)\n";
    try {
        if      (key == "platform") {
            if      (val == "mt5" || val == "MT5") cfg.platform = top4ea::PlatformType::MT5;
            else if (val == "mt4" || val == "MT4") cfg.platform = top4ea::PlatformType::MT4;
            else                                   cfg.platform = top4ea::PlatformType::Auto;
        }
        else if (key == "symbol")           cfg.symbol              = val;
        else if (key == "base_lot")         cfg.baseLot             = std::stod(val);
        else if (key == "lot_multiplier")   cfg.lotMultiplier       = std::stod(val);
        else if (key == "lot_multiplier_max") cfg.lotMultiplierMax  = std::stod(val);
        else if (key == "grid_gap")         cfg.gridGap             = std::stoi(val);
        else if (key == "grid_gap_pips")    cfg.gridGapPips         = std::stod(val);
        else if (key == "basket_target")    cfg.basketTarget        = std::stod(val);
        else if (key == "basket_max_loss")  cfg.basketMaxLoss       = std::stod(val);
        else if (key == "cascade_price_dispatch_pct")  cfg.cascadePriceDispatchPct  = std::stod(val);
        else if (key == "cascade_price_dispatch_pips") cfg.cascadePriceDispatchPips = std::stoi(val);
        else if (key == "cascade_spread_max_mult")     cfg.cascadeSpreadMaxMult     = std::stod(val);
        else if (key == "cascade_spread_ewma_alpha")   cfg.cascadeSpreadEwmaAlpha   = std::stod(val);
        else if (key == "max_recovery")         cfg.maxRecovery         = std::stoi(val);
        else if (key == "max_recovery_active")  cfg.maxRecoveryActive   = std::stoi(val);
        else if (key == "max_lots")             cfg.maxLots             = std::stod(val);
        else if (key == "agents_enabled")       cfg.agentsEnabled       = (val=="true"||val=="1");
        else if (key == "agent_class_by_lots")  cfg.agentClassByLots    = (val=="true"||val=="1");
        else if (key == "agent_x_lot_max")      cfg.agentXLotMax        = std::stod(val);
        else if (key == "agent_y_lot_max")      cfg.agentYLotMax        = std::stod(val);
        else if (key == "agent_x_usd_max")      cfg.agentXUsdMax        = std::stod(val);
        else if (key == "agent_y_usd_max")      cfg.agentYUsdMax        = std::stod(val);
        else if (key == "agent_x_cap_usd")      cfg.agentXCapUsd        = std::stod(val);
        else if (key == "agent_y_cap_usd")      cfg.agentYCapUsd        = std::stod(val);
        else if (key == "agent_z_defcon_usd")   cfg.agentZDefconUsd     = std::stod(val);
        else if (key == "agent_z_split_frac")  cfg.agentZSplitFrac     = std::stod(val);
        else if (key == "agent_cap_vs_flow_mult") cfg.agentCapVsFlowMult = std::stod(val);
        else if (key == "agent_cap_against_mult") cfg.agentCapAgainstMult = std::stod(val);
        else if (key == "agent_sacrifice_pct_base") cfg.agentSacrificePctBase = std::stod(val);
        else if (key == "agent_sacrifice_pct_emerg") cfg.agentSacrificePctEmerg = std::stod(val);
        else if (key == "agent_harvest_usd")    cfg.agentHarvestUsd     = std::stod(val);
        else if (key == "agent_x_cadence_ticks") cfg.agentXCadenceTicks = std::stoi(val);
        else if (key == "agent_y_cadence_ticks") cfg.agentYCadenceTicks = std::stoi(val);
        else if (key == "agent_z_cadence_ticks") cfg.agentZCadenceTicks = std::stoi(val);
        else if (key == "magic")                cfg.magic               = std::stoi(val);
        else if (key == "slippage")             cfg.slippage            = std::stoi(val);
        else if (key == "daily_target_pct")     cfg.dailyTargetPct      = std::stod(val);
        else if (key == "digits")               cfg.digits              = std::stoi(val);
        else if (key == "use_atr_grid")         cfg.useAtrGrid          = (val=="true"||val=="1");
        else if (key == "atr_period")           cfg.atrPeriod           = std::stoi(val);
        else if (key == "atr_mult")             cfg.atrMultiplier       = std::stod(val);
        else if (key == "atr_grid_min")         cfg.atrGridMin          = std::stoi(val);
        else if (key == "atr_grid_max")         cfg.atrGridMax          = std::stoi(val);
        else if (key == "atr_grid_min_pips")    cfg.atrGridMinPips      = std::stod(val);
        else if (key == "atr_grid_max_pips")    cfg.atrGridMaxPips      = std::stod(val);
        else if (key == "use_pnl_velocity")     cfg.usePnlVelocity      = (val=="true"||val=="1");
        else if (key == "velocity_alpha")       cfg.velocityAlpha       = std::stod(val);
        else if (key == "velocity_trigger")     cfg.velocityTrigger     = std::stod(val);
        else if (key == "kelly_max")            cfg.kellyMaxFraction    = std::stod(val);
        else if (key == "spread_avg_alpha")     cfg.spreadAvgAlpha      = std::stod(val);
        else if (key == "use_vabt")             cfg.useVABT             = (val=="true"||val=="1");
        else if (key == "vabt_coeff")           cfg.vabtCoeff           = std::stod(val);
        else if (key == "use_fixed_lot_schedule") cfg.useFixedLotSchedule = (val=="true"||val=="1");
        else if (key == "wave_enabled")         cfg.waveEnabled         = (val=="true"||val=="1");
        else if (key == "wave1_target")         cfg.wave1Target         = std::stod(val);
        else if (key == "wave2_target")         cfg.wave2Target         = std::stod(val);
        else if (key == "basket_recovery_threshold") cfg.basketRecoveryThreshold = std::stoi(val);
        else if (key == "basket_recovery_bonus")     cfg.basketRecoveryBonus     = std::stod(val);
        // Trail Stop adaptatif — 4 axes
        else if (key == "trail_step_pct")        cfg.trailStepPct         = std::stod(val);
        else if (key == "trail_freeze_zone")     cfg.trailFreezeZone      = std::stod(val);
        else if (key == "gap_expansion_factor")  cfg.gapExpansionFactor   = std::stod(val);
        else if (key == "trail_asymmetry_factor") cfg.trailAsymmetryFactor = std::stod(val);
        else if (key == "trail_fixed_dist_pts")  cfg.trailFixedDistPts    = std::stoi(val);
        else if (key == "trail_fixed_dist_pips") cfg.trailFixedDistPips   = std::stod(val);
        else if (key == "trail_min_change_pts")  cfg.trailMinChangePts    = std::stoi(val);
        else if (key == "trail_min_change_pips") cfg.trailMinChangePips   = std::stod(val);
        // Verrouillage profit trailing
        else if (key == "vl_enabled")            cfg.vlEnabled            = (val=="true"||val=="1");
        else if (key == "vl_activation_pts")     cfg.vlActivationPts      = std::stoi(val);
        else if (key == "vl_trail_pts")          cfg.vlTrailPts           = std::stoi(val);
        // Dual Trail NATURAL / AGGRESSIVE
        else if (key == "trail_mode_auto")       cfg.trailModeAuto        = (val=="true"||val=="1");
        else if (key == "x_lock_pips")           cfg.xLockPips            = std::stoi(val);
        else if (key == "bs_overshoot_pips")     cfg.bsOvershootPips      = std::stoi(val);
        else if (key == "aggressive_step_pts")   cfg.aggressiveStepPts    = std::stoi(val);
        else if (key == "natural_bs_dist_pts")   cfg.naturalBsDistPts     = std::stoi(val);
        else if (key == "natural_ss_dist_pts")   cfg.naturalSsDistPts     = std::stoi(val);
        else if (key == "natural_bs_dist_pips")  cfg.naturalBsDistPips    = std::stod(val);
        else if (key == "natural_ss_dist_pips")  cfg.naturalSsDistPips    = std::stod(val);
        // Dual Trail BUY (SS AGGRESSIVE miroir)
        else if (key == "x_lock_pips_buy")       cfg.xLockPipsBuy         = std::stoi(val);
        else if (key == "ss_overshoot_pips")     cfg.ssOvershootPips      = std::stoi(val);
        else if (key == "aggressive_ss_step_pts") cfg.aggressiveSsStepPts = std::stoi(val);
        // Rétrocompatibilité anciens fichiers .conf (be_ → vl_)
        else if (key == "be_enabled")            cfg.vlEnabled            = (val=="true"||val=="1");
        else if (key == "be_buffer_pts")         cfg.vlTrailPts           = std::stoi(val);
        // Connexion MetaApi
        else if (key == "tick_poll_ms")          cfg.tickPollMs           = std::stoi(val);
        else if (key == "state_poll_ms")         cfg.statePollMs          = std::stoi(val);
        else if (key == "reconnect_ms")          cfg.reconnectMs          = std::stoi(val);
        // DeepSeek
        else if (key == "ds_stale_hard_sec")     cfg.dsStaleHardSec       = std::stoi(val);
        else if (key == "deepseek_timeout_sec")  cfg.deepseekTimeoutSec   = std::stoi(val);
        // Vote board gate
        else if (key == "vote_ss_only_threshold") cfg.voteSsOnlyThreshold  = std::stoi(val);
        else if (key == "vote_bs_only_threshold") cfg.voteBsOnlyThreshold  = std::stoi(val);
        else if (key == "ds_hard_gate_pct")       cfg.dsHardGatePct        = std::stod(val);
        // Legacy aliases
        else if (key == "vote_gate_min_score") {
            int threshold = std::stoi(val);
            cfg.voteSsOnlyThreshold = -std::abs(threshold);
            cfg.voteBsOnlyThreshold =  std::abs(threshold);
        }
        else if (key == "vote_gate_ds_conf_high") cfg.voteGateDsConfHigh   = std::stoi(val);
        else if (key == "vote_gate_ds_conf_low")  cfg.voteGateDsConfLow    = std::stoi(val);
    } catch (...) {
        std::cerr << "[Config] Erreur parsing: " << key << "=" << val << "\n";
    }
}

// ─── Table de correspondance clé conf → variable d'environnement ──
// Format : TOP4EA_<NOM_MAJUSCULE> — ex: grid_gap → TOP4EA_GRID_GAP
static const std::vector<std::pair<std::string,std::string>> ENV_KEYS = {
    {"platform",                    "TOP4EA_PLATFORM"},
    {"symbol",                      "TOP4EA_SYMBOL"},
    {"base_lot",                    "TOP4EA_BASE_LOT"},
    {"lot_multiplier",              "TOP4EA_LOT_MULTIPLIER"},
    {"lot_multiplier_max",          "TOP4EA_LOT_MULTIPLIER_MAX"},
    {"grid_gap",                    "TOP4EA_GRID_GAP"},
    {"grid_gap_pips",               "TOP4EA_GRID_GAP_PIPS"},
    {"basket_target",               "TOP4EA_BASKET_TARGET"},
    {"basket_max_loss",             "TOP4EA_BASKET_MAX_LOSS"},
    {"max_recovery",                "TOP4EA_MAX_RECOVERY"},
    {"max_recovery_active",         "TOP4EA_MAX_RECOVERY_ACTIVE"},
    {"max_lots",                    "TOP4EA_MAX_LOTS"},
    {"agents_enabled",              "TOP4EA_AGENTS_ENABLED"},
    {"agent_class_by_lots",         "TOP4EA_AGENT_CLASS_BY_LOTS"},
    {"agent_x_lot_max",             "TOP4EA_AGENT_X_LOT_MAX"},
    {"agent_y_lot_max",             "TOP4EA_AGENT_Y_LOT_MAX"},
    {"agent_x_usd_max",             "TOP4EA_AGENT_X_USD_MAX"},
    {"agent_y_usd_max",             "TOP4EA_AGENT_Y_USD_MAX"},
    {"agent_x_cap_usd",             "TOP4EA_AGENT_X_CAP_USD"},
    {"agent_y_cap_usd",             "TOP4EA_AGENT_Y_CAP_USD"},
    {"agent_z_defcon_usd",          "TOP4EA_AGENT_Z_DEFCON_USD"},
    {"agent_z_split_frac",          "TOP4EA_AGENT_Z_SPLIT_FRAC"},
    {"agent_cap_vs_flow_mult",      "TOP4EA_AGENT_CAP_VS_FLOW_MULT"},
    {"agent_cap_against_mult",      "TOP4EA_AGENT_CAP_AGAINST_MULT"},
    {"agent_sacrifice_pct_base",    "TOP4EA_AGENT_SACRIFICE_PCT_BASE"},
    {"agent_sacrifice_pct_emerg",   "TOP4EA_AGENT_SACRIFICE_PCT_EMERG"},
    {"agent_harvest_usd",           "TOP4EA_AGENT_HARVEST_USD"},
    {"agent_x_cadence_ticks",       "TOP4EA_AGENT_X_CADENCE_TICKS"},
    {"agent_y_cadence_ticks",       "TOP4EA_AGENT_Y_CADENCE_TICKS"},
    {"agent_z_cadence_ticks",       "TOP4EA_AGENT_Z_CADENCE_TICKS"},
    {"magic",                       "TOP4EA_MAGIC"},
    {"slippage",                    "TOP4EA_SLIPPAGE"},
    {"daily_target_pct",            "TOP4EA_DAILY_TARGET_PCT"},
    {"digits",                      "TOP4EA_DIGITS"},
    {"use_atr_grid",                "TOP4EA_USE_ATR_GRID"},
    {"atr_period",                  "TOP4EA_ATR_PERIOD"},
    {"atr_mult",                    "TOP4EA_ATR_MULT"},
    {"atr_grid_min",                "TOP4EA_ATR_GRID_MIN"},
    {"atr_grid_max",                "TOP4EA_ATR_GRID_MAX"},
    {"atr_grid_min_pips",           "TOP4EA_ATR_GRID_MIN_PIPS"},
    {"atr_grid_max_pips",           "TOP4EA_ATR_GRID_MAX_PIPS"},
    {"use_pnl_velocity",            "TOP4EA_USE_PNL_VELOCITY"},
    {"velocity_alpha",              "TOP4EA_VELOCITY_ALPHA"},
    {"velocity_trigger",            "TOP4EA_VELOCITY_TRIGGER"},
    {"kelly_max",                   "TOP4EA_KELLY_MAX"},
    {"spread_avg_alpha",            "TOP4EA_SPREAD_AVG_ALPHA"},
    {"use_vabt",                    "TOP4EA_USE_VABT"},
    {"vabt_coeff",                  "TOP4EA_VABT_COEFF"},
    {"use_fixed_lot_schedule",      "TOP4EA_USE_FIXED_LOT_SCHEDULE"},
    {"wave_enabled",                "TOP4EA_WAVE_ENABLED"},
    {"wave1_target",                "TOP4EA_WAVE1_TARGET"},
    {"wave2_target",                "TOP4EA_WAVE2_TARGET"},
    {"basket_recovery_threshold",   "TOP4EA_BASKET_RECOVERY_THRESHOLD"},
    {"basket_recovery_bonus",       "TOP4EA_BASKET_RECOVERY_BONUS"},
    {"trail_step_pct",              "TOP4EA_TRAIL_STEP_PCT"},
    {"trail_freeze_zone",           "TOP4EA_TRAIL_FREEZE_ZONE"},
    {"gap_expansion_factor",        "TOP4EA_GAP_EXPANSION_FACTOR"},
    {"trail_asymmetry_factor",      "TOP4EA_TRAIL_ASYMMETRY_FACTOR"},
    {"trail_fixed_dist_pts",        "TOP4EA_TRAIL_FIXED_DIST_PTS"},
    {"trail_fixed_dist_pips",       "TOP4EA_TRAIL_FIXED_DIST_PIPS"},
    {"trail_min_change_pts",        "TOP4EA_TRAIL_MIN_CHANGE_PTS"},
    {"trail_min_change_pips",       "TOP4EA_TRAIL_MIN_CHANGE_PIPS"},
    {"vl_enabled",                  "TOP4EA_VL_ENABLED"},
    {"vl_activation_pts",           "TOP4EA_VL_ACTIVATION_PTS"},
    {"vl_trail_pts",                "TOP4EA_VL_TRAIL_PTS"},
    {"trail_mode_auto",             "TOP4EA_TRAIL_MODE_AUTO"},
    {"x_lock_pips",                 "TOP4EA_X_LOCK_PIPS"},
    {"bs_overshoot_pips",           "TOP4EA_BS_OVERSHOOT_PIPS"},
    {"aggressive_step_pts",         "TOP4EA_AGGRESSIVE_STEP_PTS"},
    {"natural_bs_dist_pts",         "TOP4EA_NATURAL_BS_DIST_PTS"},
    {"natural_ss_dist_pts",         "TOP4EA_NATURAL_SS_DIST_PTS"},
    {"natural_bs_dist_pips",        "TOP4EA_NATURAL_BS_DIST_PIPS"},
    {"natural_ss_dist_pips",        "TOP4EA_NATURAL_SS_DIST_PIPS"},
    {"x_lock_pips_buy",             "TOP4EA_X_LOCK_PIPS_BUY"},
    {"ss_overshoot_pips",           "TOP4EA_SS_OVERSHOOT_PIPS"},
    {"aggressive_ss_step_pts",      "TOP4EA_AGGRESSIVE_SS_STEP_PTS"},
    {"cascade_price_dispatch_pct",  "TOP4EA_CASCADE_PRICE_DISPATCH_PCT"},
    {"cascade_price_dispatch_pips", "TOP4EA_CASCADE_PRICE_DISPATCH_PIPS"},
    {"cascade_spread_max_mult",     "TOP4EA_CASCADE_SPREAD_MAX_MULT"},
    {"cascade_spread_ewma_alpha",   "TOP4EA_CASCADE_SPREAD_EWMA_ALPHA"},
    {"tick_poll_ms",                "TOP4EA_TICK_POLL_MS"},
    {"state_poll_ms",               "TOP4EA_STATE_POLL_MS"},
    {"reconnect_ms",                "TOP4EA_RECONNECT_MS"},
    {"ds_stale_hard_sec",           "TOP4EA_DS_STALE_HARD_SEC"},
    {"deepseek_timeout_sec",        "TOP4EA_DEEPSEEK_TIMEOUT_SEC"},
    {"vote_ss_only_threshold",      "TOP4EA_VOTE_SS_ONLY_THRESHOLD"},
    {"vote_bs_only_threshold",      "TOP4EA_VOTE_BS_ONLY_THRESHOLD"},
    {"ds_hard_gate_pct",            "TOP4EA_DS_HARD_GATE_PCT"},
    // Legacy aliases
    {"vote_gate_min_score",         "TOP4EA_VOTE_GATE_MIN_SCORE"},
    {"vote_gate_ds_conf_high",      "TOP4EA_VOTE_GATE_DS_CONF_HIGH"},
    {"vote_gate_ds_conf_low",       "TOP4EA_VOTE_GATE_DS_CONF_LOW"},
};

// ─── Lecture config depuis fichier INI simplifié ─────────────
top4ea::Config loadConfig(const std::string& path) {
    top4ea::Config cfg;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cout << "[Config] Fichier '" << path
                  << "' non trouvé — paramètres par défaut\n";
    } else {
        std::string line;
        auto trim = [](std::string s) -> std::string {
            size_t b = s.find_first_not_of(" \t\r\n");
            size_t e = s.find_last_not_of(" \t\r\n");
            return (b == std::string::npos) ? "" : s.substr(b, e-b+1);
        };
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq+1));
            applyConfigKV(cfg, key, val);
        }
        std::cout << "[Config] Chargé depuis '" << path << "'\n";
    }

    // ── Surcharges variables d'environnement (env-first) ──────
    // Toute variable TOP4EA_<PARAM> non-vide écrase la valeur du fichier.
    for (const auto& [confKey, envKey] : ENV_KEYS) {
        std::string ev = getEnvOr(envKey.c_str());
        if (!ev.empty())
            applyConfigKV(cfg, confKey, ev, /*logOverride=*/true);
    }

    // digits=0 → auto-détection broker au démarrage (recommandé)
    // digits>0 → surcharge manuelle (override broker)
    if (cfg.digits > 0) {
        cfg.point = std::pow(10.0, -(double)cfg.digits);
        std::cout << "[Config] digits=" << cfg.digits
                  << " (surcharge manuelle) point=" << cfg.point << "\n";
    } else {
        std::cout << "[Config] digits=0 → auto-détection broker activée\n";
    }
    return cfg;
}

// ─── Affichage bannière ──────────────────────────────────────
void printBanner(const top4ea::Config& cfg) {
    const char* platStr = (cfg.platform == top4ea::PlatformType::MT5) ? "MT5"
                        : (cfg.platform == top4ea::PlatformType::MT4) ? "MT4"
                        : "Auto (détection au démarrage)";
    std::cout << R"(
╔══════════════════════════════════════════════════╗
║        TOP4EA  C++20  v3.1  — INSTITUTIONNEL     ║
║   Grid Recovery EA — MetaApi MT4 / MT5           ║
╠══════════════════════════════════════════════════╣
)" << "║  Plateforme: " << platStr      << "\n"
   << "║  Symbole   : " << cfg.symbol       << "\n"
   << "║  Digits    : " << (cfg.digits==0 ? "0 (AUTO-DÉTECTION BROKER)" :
                             std::to_string(cfg.digits) + " (surcharge manuelle)") << "\n"
   << "║  BaseLot   : " << cfg.baseLot      << "\n"
   << "║  LotMult   : " << cfg.lotMultiplier << " → " << cfg.lotMultiplierMax
      << (cfg.useFixedLotSchedule ? " [schedule fixe]" : " [gradient actif]") << "\n"
   << "║  GridGap   : " << cfg.gridGap      << " pts (sera rescalé si broker 3-digit)\n"
   << "║  ATRGrid   : " << (cfg.useAtrGrid?"ON":"OFF") << "\n"
   << "║  Target    : " << cfg.basketTarget << " $\n"
   << "║  MaxLoss   : " << cfg.basketMaxLoss << " $ (urgence MaxRecoveryHold)\n"
   << "║  MaxRecov  : " << cfg.maxRecovery  << "\n"
   << "║  TickPoll  : " << cfg.tickPollMs << " ms\n"
   << R"(╚══════════════════════════════════════════════════╝
)";
}

// ─── Main ───────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);

    std::string cfgPath = "top4ea.conf";
    if (argc > 1) cfgPath = argv[1];

    auto cfg = loadConfig(cfgPath);
    printBanner(cfg);

    top4ea::TOP4EA_Engine engine(std::move(cfg));
    engine.start();

    std::cout << "[TOP4EA] Moteur démarré — Ctrl+C pour arrêter\n";
    std::cout << "[TOP4EA] Connexion MetaApi en cours...\n";

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n[TOP4EA] Arrêt propre en cours...\n";
    engine.stop();
    std::cout << "[TOP4EA] Arrêté.\n";
    return 0;
}
