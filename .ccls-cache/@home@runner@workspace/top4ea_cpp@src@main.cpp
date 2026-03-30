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

// ─── Signal handler pour arrêt propre ───────────────────────
static std::atomic<bool> g_running { true };

static void sigHandler(int) {
    g_running.store(false);
}

// ─── Lecture config depuis fichier INI simplifié ─────────────
top4ea::Config loadConfig(const std::string& path) {
    top4ea::Config cfg;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cout << "[Config] Fichier '" << path
                  << "' non trouvé — paramètres par défaut\n";
        return cfg;
    }
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
        try {
            if      (key == "symbol")           cfg.symbol           = val;
            else if (key == "base_lot")         cfg.baseLot          = std::stod(val);
            else if (key == "lot_multiplier")   cfg.lotMultiplier    = std::stod(val);
            else if (key == "grid_gap")         cfg.gridGap          = std::stoi(val);
            else if (key == "basket_target")    cfg.basketTarget     = std::stod(val);
            else if (key == "cascade_price_dispatch_pct") cfg.cascadePriceDispatchPct = std::stod(val);
            else if (key == "cascade_spread_max_mult")    cfg.cascadeSpreadMaxMult    = std::stod(val);
            else if (key == "cascade_spread_ewma_alpha")  cfg.cascadeSpreadEwmaAlpha  = std::stod(val);
            else if (key == "max_recovery")     cfg.maxRecovery      = std::stoi(val);
            else if (key == "magic")            cfg.magic            = std::stoi(val);
            else if (key == "slippage")         cfg.slippage         = std::stoi(val);
            else if (key == "daily_target_pct") cfg.dailyTargetPct   = std::stod(val);
            else if (key == "digits")           cfg.digits           = std::stoi(val);
            else if (key == "use_atr_grid")     cfg.useAtrGrid       = (val=="true"||val=="1");
            else if (key == "atr_period")       cfg.atrPeriod        = std::stoi(val);
            else if (key == "atr_mult")         cfg.atrMultiplier    = std::stod(val);
            else if (key == "atr_grid_min")     cfg.atrGridMin       = std::stoi(val);
            else if (key == "atr_grid_max")     cfg.atrGridMax       = std::stoi(val);
            else if (key == "use_pnl_velocity") cfg.usePnlVelocity   = (val=="true"||val=="1");
            else if (key == "velocity_alpha")   cfg.velocityAlpha    = std::stod(val);
            else if (key == "velocity_trigger") cfg.velocityTrigger  = std::stod(val);
            else if (key == "kelly_max")            cfg.kellyMaxFraction    = std::stod(val);
            else if (key == "spread_avg_alpha")     cfg.spreadAvgAlpha      = std::stod(val);
            // Trail Stop adaptatif — 4 axes
            else if (key == "trail_step_pct")       cfg.trailStepPct        = std::stod(val);
            else if (key == "trail_freeze_zone")    cfg.trailFreezeZone     = std::stod(val);
            else if (key == "gap_expansion_factor") cfg.gapExpansionFactor  = std::stod(val);
            else if (key == "trail_asymmetry_factor") cfg.trailAsymmetryFactor = std::stod(val);
            else if (key == "bridge_host")      cfg.bridgeHost       = val;
            else if (key == "bridge_port")      cfg.bridgePort       = (uint16_t)std::stoi(val);
            else if (key == "reconnect_ms")     cfg.reconnectMs      = std::stoi(val);
            else if (key == "ping_interval_ms") cfg.pingIntervalMs   = std::stoi(val);
        } catch (...) {
            std::cerr << "[Config] Erreur parsing: " << key << "=" << val << "\n";
        }
    }
    std::cout << "[Config] Chargé depuis '" << path << "'\n";
    return cfg;
}

// ─── Affichage bannière ──────────────────────────────────────
void printBanner(const top4ea::Config& cfg) {
    std::cout << R"(
╔══════════════════════════════════════════════════╗
║           TOP4EA  C++20  v1.0                    ║
║   Grid Recovery EA — Connexion MT4 TCP Bridge    ║
╠══════════════════════════════════════════════════╣
)" << "║  Symbole   : " << cfg.symbol       << "\n"
   << "║  BaseLot   : " << cfg.baseLot      << "\n"
   << "║  LotMult   : " << cfg.lotMultiplier<< "\n"
   << "║  GridGap   : " << cfg.gridGap      << " pts\n"
   << "║  ATRGrid   : " << (cfg.useAtrGrid?"ON":"OFF") << "\n"
   << "║  Target    : " << cfg.basketTarget << " $\n"
   << "║  MaxRecov  : " << cfg.maxRecovery  << "\n"
   << "║  Daily%    : " << cfg.dailyTargetPct << "%\n"
   << "║  Bridge    : " << cfg.bridgeHost << ":" << cfg.bridgePort << "\n"
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
    std::cout << "[TOP4EA] En attente de connexion MT4 Bridge sur port "
              << cfg.bridgePort << "...\n";

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n[TOP4EA] Arrêt propre en cours...\n";
    engine.stop();
    std::cout << "[TOP4EA] Arrêté.\n";
    return 0;
}
