#pragma once
// ============================================================
//  TOP4EA — types.hpp
//  Définitions centrales partagées — C++20
// ============================================================
#include <cstdint>
#include <string>
#include <chrono>
#include <atomic>
#include <array>

namespace top4ea {

// ─── Horodatage haute résolution ────────────────────────────
using Clock     = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;
using Ms        = std::chrono::milliseconds;
using Us        = std::chrono::microseconds;

// ─── Types de marché MT4 ────────────────────────────────────
enum class OrderType : int {
    Buy       = 0,
    Sell      = 1,
    BuyLimit  = 2,
    SellLimit = 3,
    BuyStop   = 4,
    SellStop  = 5,
    None      = -1
};

// ─── États internes de la machine ───────────────────────────
enum class EAState : uint8_t {
    Init        = 0,
    GridActive  = 1,
    Recovery    = 2,
    Closing     = 3,
    DailyPause  = 4
};

// ─── Régime de volatilité (calcul ATR) ──────────────────────
enum class VolRegime : uint8_t {
    Low    = 0,   // ATR < seuil bas
    Medium = 1,   // normal
    High   = 2    // ATR > seuil haut
};

// ─── Ticket + métadonnées d'un ordre ────────────────────────
struct OrderInfo {
    int        ticket     = -1;
    OrderType  type       = OrderType::None;
    double     lots       = 0.0;
    double     openPrice  = 0.0;
    double     sl         = 0.0;
    double     tp         = 0.0;
    double     profit     = 0.0;    // P&L courant (profit+swap+commission)
    double     swap       = 0.0;
    double     commission = 0.0;
    TimePoint  openTime   = {};
    std::string comment   = {};
};

// ─── Tick de marché reçu depuis MT4 ─────────────────────────
struct Tick {
    double    bid        = 0.0;
    double    ask        = 0.0;
    double    spread     = 0.0;
    TimePoint serverTime = {};
    TimePoint localTime  = {};
};

// ─── Snapshot complet du compte ─────────────────────────────
struct AccountSnapshot {
    double balance      = 0.0;
    double equity       = 0.0;
    double freeMargin   = 0.0;
    double marginLevel  = 0.0;
    double marginReq    = 0.0;     // par lot
    int    leverage     = 0;
    std::string currency = {};
};

// ─── Paramètres configurables (équivalent extern MQL4) ──────
struct Config {
    std::string symbol          = "XAUUSDm";
    double      baseLot         = 0.10;
    double      lotMultiplier   = 1.15;
    int         gridGap         = 500;          // points – valeur initiale
    double      basketTarget    = 7.0;          // $ fixe
    int         maxRecovery     = 20;

    // Garde cascade professionnel (remplace antiCascadeSec)
    double      cascadePriceDispatchPct = 0.50; // 50% du GridGap comme déplacement min
    double      cascadeSpreadMaxMult    = 2.5;  // spread max = 2.5× spread moyen
    int         cascadeMomentumTicks    = 3;    // N ticks consécutifs requis
    double      cascadeSpreadEwmaAlpha  = 0.05; // lissage spread référence
    int         magic           = 10300;
    int         slippage        = 3;
    double      dailyTargetPct  = 10.0;         // 0 = désactivé
    int         digits          = 5;
    double      point           = 0.00001;

    // Améliorations mathématiques
    bool        useAtrGrid      = true;         // GridGap dynamique ATR
    int         atrPeriod       = 14;
    double      atrMultiplier   = 0.5;          // gap = ATR * mult
    double      atrGridMin      = 200;          // plancher points
    double      atrGridMax      = 1000;         // plafond points

    bool        usePnlVelocity  = true;         // fermeture prédictive
    double      velocityAlpha   = 0.3;          // EWMA α
    double      velocityTrigger = 0.85;         // fermeture si pnl >= target*pct

    double      kellyMaxFraction = 0.25;        // cap Kelly lot recovery
    double      spreadAvgAlpha   = 0.1;         // EWMA spread historique

    // Connexion MT4 bridge
    std::string bridgeHost      = "127.0.0.1";
    uint16_t    bridgePort      = 9000;
    int         reconnectMs     = 1000;
    int         pingIntervalMs  = 2000;
};

// ─── Résultat d'une opération de trading ────────────────────
struct TradeResult {
    bool    success  = false;
    int     ticket   = -1;
    int     error    = 0;
    double  price    = 0.0;
    std::string msg  = {};
};

} // namespace top4ea
