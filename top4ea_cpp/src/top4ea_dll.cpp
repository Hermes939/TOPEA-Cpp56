// ============================================================
//  TOP4EA — top4ea_dll.cpp  C++20
//  Implémentation de l'API C exportée (DLL / .so)
//
//  Ce fichier est le point d'entrée DLL — il fait le pont
//  entre l'API C (compatible MT4/MQL4) et le moteur C++20.
// ============================================================
#include "../include/top4ea_dll.h"
#include "../include/top4ea_engine.hpp"
#include <fstream>
#include <sstream>
#include <string>
#include <chrono>
#include <cstring>
#include <iostream>
#include <cmath>

// ─── Handle interne ─────────────────────────────────────────
struct TOP4EA_Handle {
    top4ea::Config        cfg;
    top4ea::TOP4EA_Engine engine;
    std::atomic<bool>     running { false };

    explicit TOP4EA_Handle(top4ea::Config c)
        : cfg(c), engine(std::move(c)) {}
};

// ─── Lecture config INI ─────────────────────────────────────
static top4ea::Config loadConfig(const char* path) {
    top4ea::Config cfg;
    if (!path || !*path) return cfg;

    std::ifstream f(path);
    if (!f.is_open()) {
        std::cout << "[DLL] Config '" << path << "' non trouvé — défauts\n";
        return cfg;
    }

    auto trim = [](std::string s) -> std::string {
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of (" \t\r\n");
        return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
    };

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0]=='#' || line[0]==';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        try {
            if      (key=="platform") {
                if      (val=="mt5"||val=="MT5") cfg.platform = top4ea::PlatformType::MT5;
                else if (val=="mt4"||val=="MT4") cfg.platform = top4ea::PlatformType::MT4;
                else                             cfg.platform = top4ea::PlatformType::Auto;
            }
            else if (key=="symbol")           cfg.symbol            = val;
            else if (key=="base_lot")          cfg.baseLot            = std::stod(val);
            else if (key=="lot_multiplier")    cfg.lotMultiplier      = std::stod(val);
            else if (key=="lot_multiplier_max") cfg.lotMultiplierMax  = std::stod(val);
            else if (key=="grid_gap")         cfg.gridGap           = std::stoi(val);
            else if (key=="basket_target")    cfg.basketTarget      = std::stod(val);
            else if (key=="max_recovery")     cfg.maxRecovery       = std::stoi(val);
            else if (key=="magic")            cfg.magic             = std::stoi(val);
            else if (key=="slippage")         cfg.slippage          = std::stoi(val);
            else if (key=="daily_target_pct") cfg.dailyTargetPct    = std::stod(val);
            else if (key=="digits")           cfg.digits            = std::stoi(val);
            else if (key=="use_atr_grid")     cfg.useAtrGrid        = (val=="true"||val=="1");
            else if (key=="atr_period")       cfg.atrPeriod         = std::stoi(val);
            else if (key=="atr_mult")         cfg.atrMultiplier     = std::stod(val);
            else if (key=="atr_grid_min")     cfg.atrGridMin        = std::stoi(val);
            else if (key=="atr_grid_max")     cfg.atrGridMax        = std::stoi(val);
            else if (key=="use_pnl_velocity") cfg.usePnlVelocity    = (val=="true"||val=="1");
            else if (key=="velocity_alpha")   cfg.velocityAlpha     = std::stod(val);
            else if (key=="velocity_trigger") cfg.velocityTrigger   = std::stod(val);
            else if (key=="kelly_max")        cfg.kellyMaxFraction  = std::stod(val);
            else if (key=="spread_avg_alpha") cfg.spreadAvgAlpha    = std::stod(val);
            else if (key=="trail_step_pct")   cfg.trailStepPct      = std::stod(val);
            else if (key=="trail_freeze_zone")cfg.trailFreezeZone   = std::stod(val);
            else if (key=="gap_expansion_factor")  cfg.gapExpansionFactor  = std::stod(val);
            else if (key=="trail_asymmetry_factor") cfg.trailAsymmetryFactor= std::stod(val);
            else if (key=="trail_fixed_dist_pts") cfg.trailFixedDistPts   = std::stoi(val);
            else if (key=="trail_min_change_pts") cfg.trailMinChangePts   = std::stoi(val);
            else if (key=="cascade_price_dispatch_pct")  cfg.cascadePriceDispatchPct  = std::stod(val);
            else if (key=="cascade_price_dispatch_pips") cfg.cascadePriceDispatchPips = std::stoi(val);
            else if (key=="cascade_spread_max_mult")    cfg.cascadeSpreadMaxMult    = std::stod(val);
            else if (key=="cascade_spread_ewma_alpha")  cfg.cascadeSpreadEwmaAlpha  = std::stod(val);
            // Basket fermeture contextualisée
            else if (key=="basket_recovery_threshold") cfg.basketRecoveryThreshold = std::stoi(val);
            else if (key=="basket_recovery_bonus")     cfg.basketRecoveryBonus     = std::stod(val);
            // Schedule lots fixe recovery
            else if (key=="use_fixed_lot_schedule")   cfg.useFixedLotSchedule     = (val=="true"||val=="1");
            // TP par vagues
            else if (key=="wave_enabled")   cfg.waveEnabled   = (val=="true"||val=="1");
            else if (key=="wave1_target")   cfg.wave1Target   = std::stod(val);
            else if (key=="wave2_target")   cfg.wave2Target   = std::stod(val);
            else if (key=="tick_poll_ms")     cfg.tickPollMs        = std::stoi(val);
            else if (key=="state_poll_ms")    cfg.statePollMs       = std::stoi(val);
            else if (key=="reconnect_ms")     cfg.reconnectMs       = std::stoi(val);
        } catch (...) {}
    }
    // digits=0 → auto-détection broker au démarrage
    // digits>0 → surcharge manuelle — on calcule le point ici
    if (cfg.digits > 0) {
        cfg.point = std::pow(10.0, -(double)cfg.digits);
        std::cout << "[DLL] Config chargé : " << path
                  << " | digits=" << cfg.digits
                  << " (surcharge manuelle) point=" << cfg.point << "\n";
    } else {
        std::cout << "[DLL] Config chargé : " << path
                  << " | digits=0 → auto-détection broker activée\n";
    }
    return cfg;
}

// ============================================================
//  API C EXPORTÉE
// ============================================================

extern "C" {

// ─── Création / Destruction ──────────────────────────────────
void* TOP4EA_Create(const char* config_path) {
    try {
        auto cfg = loadConfig(config_path);
        auto* h  = new TOP4EA_Handle(std::move(cfg));
        std::cout << "[DLL] TOP4EA_Create OK\n";
        return h;
    } catch (const std::exception& e) {
        std::cerr << "[DLL] TOP4EA_Create erreur: " << e.what() << "\n";
        return nullptr;
    }
}

void TOP4EA_Destroy(void* handle) {
    if (!handle) return;
    auto* h = static_cast<TOP4EA_Handle*>(handle);
    if (h->running.load()) h->engine.stop();
    delete h;
    std::cout << "[DLL] TOP4EA_Destroy OK\n";
}

void TOP4EA_Start(void* handle) {
    if (!handle) return;
    auto* h = static_cast<TOP4EA_Handle*>(handle);
    if (!h->running.exchange(true)) {
        h->engine.start();
        std::cout << "[DLL] TOP4EA_Start OK\n";
    }
}

void TOP4EA_Stop(void* handle) {
    if (!handle) return;
    auto* h = static_cast<TOP4EA_Handle*>(handle);
    if (h->running.exchange(false)) {
        h->engine.stop();
        std::cout << "[DLL] TOP4EA_Stop OK\n";
    }
}

// ─── État ────────────────────────────────────────────────────
int TOP4EA_IsConnected(void* handle) {
    if (!handle) return 0;
    auto* h = static_cast<TOP4EA_Handle*>(handle);
    return h->engine.isConnected() ? 1 : 0;
}

int TOP4EA_IsRunning(void* handle) {
    if (!handle) return 0;
    auto* h = static_cast<TOP4EA_Handle*>(handle);
    return h->running.load() ? 1 : 0;
}

void TOP4EA_GetStatus(void* handle, char* buf, int buf_len) {
    if (!handle || !buf || buf_len <= 0) return;
    auto* h = static_cast<TOP4EA_Handle*>(handle);
    std::string s = h->engine.dashboard();
    std::strncpy(buf, s.c_str(), static_cast<size_t>(buf_len - 1));
    buf[buf_len - 1] = '\0';
}

// ─── Injection tick (mode EA MQL4 sans MetaApi) ─────────────
void TOP4EA_OnTick(void* handle,
                   const char* symbol,
                   double bid,
                   double ask,
                   long long time_ms) {
    if (!handle) return;
    (void)symbol;
    (void)time_ms;  // serverTime est steady_clock, non convertible depuis Unix ms
    auto* h = static_cast<TOP4EA_Handle*>(handle);
    top4ea::Tick t;
    t.bid       = bid;
    t.ask       = ask;
    t.spread    = ask - bid;
    t.localTime = top4ea::Clock::now();
    h->engine.injectTick(t);
}

// ─── Commandes trading directes ──────────────────────────────
int TOP4EA_PlaceOrder(void* handle,
                      int    order_type,
                      double lots,
                      double price,
                      double sl,
                      double tp,
                      int    magic,
                      int    slippage,
                      const char* comment) {
    if (!handle) return -1;
    auto* h = static_cast<TOP4EA_Handle*>(handle);

    top4ea::TradeCmd cmd;
    cmd.op       = top4ea::TradeCmd::Op::MarketOrder;
    cmd.lots     = lots;
    cmd.price    = price;
    cmd.sl       = sl;
    cmd.tp       = tp;
    cmd.magic    = magic;
    cmd.slippage = slippage;
    cmd.comment  = comment ? comment : "";

    switch (order_type) {
        case TOP4EA_OP_BUY:       cmd.type = top4ea::OrderType::Buy;       break;
        case TOP4EA_OP_SELL:      cmd.type = top4ea::OrderType::Sell;      break;
        case TOP4EA_OP_BUYLIMIT:  cmd.type = top4ea::OrderType::BuyLimit;  break;
        case TOP4EA_OP_SELLLIMIT: cmd.type = top4ea::OrderType::SellLimit; break;
        case TOP4EA_OP_BUYSTOP:   cmd.type = top4ea::OrderType::BuyStop;   break;
        case TOP4EA_OP_SELLSTOP:  cmd.type = top4ea::OrderType::SellStop;  break;
        default:                  cmd.type = top4ea::OrderType::Buy;       break;
    }

    if (order_type == TOP4EA_OP_BUYSTOP || order_type == TOP4EA_OP_SELLSTOP ||
        order_type == TOP4EA_OP_BUYLIMIT || order_type == TOP4EA_OP_SELLLIMIT)
        cmd.op = top4ea::TradeCmd::Op::MarketOrder;

    auto res = h->engine.sendTrade(cmd);
    return res.success ? res.ticket : -(res.error);
}

int TOP4EA_CloseOrder(void* handle, int ticket, double lots,
                      double price, int /*slippage*/) {
    if (!handle) return -1;
    auto* h = static_cast<TOP4EA_Handle*>(handle);
    (void)lots; (void)price;

    top4ea::TradeCmd cmd;
    cmd.op     = top4ea::TradeCmd::Op::CloseOrder;
    cmd.ticket = ticket;

    auto res = h->engine.sendTrade(cmd);
    return res.success ? 1 : -(res.error);
}

int TOP4EA_DeleteOrder(void* handle, int ticket) {
    if (!handle) return -1;
    auto* h = static_cast<TOP4EA_Handle*>(handle);

    top4ea::TradeCmd cmd;
    cmd.op     = top4ea::TradeCmd::Op::DeletePending;
    cmd.ticket = ticket;

    auto res = h->engine.sendTrade(cmd);
    return res.success ? 1 : -(res.error);
}

int TOP4EA_ModifyOrder(void* handle, int ticket, double price,
                       double sl, double tp) {
    if (!handle) return -1;
    auto* h = static_cast<TOP4EA_Handle*>(handle);

    top4ea::TradeCmd cmd;
    cmd.op     = top4ea::TradeCmd::Op::ModifyOrder;  // ordre pending
    cmd.ticket = ticket;
    cmd.price  = price;
    cmd.sl     = sl;
    cmd.tp     = tp;

    auto res = h->engine.sendTrade(cmd);
    return res.success ? 1 : -(res.error);
}

// ── Modification SL/TP d'une position ouverte (MT5 : POSITION_MODIFY) ─
// À utiliser pour modifier les stops d'un trade actif (Buy/Sell)
// Différent de ModifyOrder qui s'applique aux ordres pending uniquement.
int TOP4EA_ModifyPosition(void* handle, int ticket, double sl, double tp) {
    if (!handle) return -1;
    auto* h = static_cast<TOP4EA_Handle*>(handle);

    top4ea::TradeCmd cmd;
    cmd.op     = top4ea::TradeCmd::Op::ModifyPosition;
    cmd.ticket = ticket;
    cmd.price  = 0.0;
    cmd.sl     = sl;
    cmd.tp     = tp;

    auto res = h->engine.sendTrade(cmd);
    return res.success ? 1 : -(res.error);
}

// ─── Infos compte ────────────────────────────────────────────
double TOP4EA_GetBalance(void* handle) {
    if (!handle) return 0.0;
    return static_cast<TOP4EA_Handle*>(handle)->engine.getBalance();
}

double TOP4EA_GetEquity(void* handle) {
    if (!handle) return 0.0;
    return static_cast<TOP4EA_Handle*>(handle)->engine.getEquity();
}

double TOP4EA_GetFreeMargin(void* handle) {
    if (!handle) return 0.0;
    return static_cast<TOP4EA_Handle*>(handle)->engine.getFreeMargin();
}

double TOP4EA_GetDailyPnL(void* handle) {
    if (!handle) return 0.0;
    return static_cast<TOP4EA_Handle*>(handle)->engine.getDailyPnL();
}

int TOP4EA_GetOrderCount(void* handle) {
    if (!handle) return 0;
    return static_cast<TOP4EA_Handle*>(handle)->engine.getOrderCount();
}

// ─── Fermeture globale ───────────────────────────────────────
int TOP4EA_CloseAll(void* handle) {
    if (!handle) return -1;
    auto* h = static_cast<TOP4EA_Handle*>(handle);
    try {
        h->engine.closeAllOrders();
        std::cout << "[DLL] TOP4EA_CloseAll OK\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[DLL] TOP4EA_CloseAll erreur: " << e.what() << "\n";
        return -1;
    }
}

// ─── Version ─────────────────────────────────────────────────
const char* TOP4EA_Version(void) {
    return "TOP4EA C++20 DLL v3.0 (V3/v6.04)";
}

} // extern "C"
