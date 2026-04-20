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
    int         ticket     = -1;   // int synthétique stable (hash ou stoi)
    std::string id         = {};   // ID MetaApi brut (UUID ou numérique string)
    OrderType   type       = OrderType::None;
    double      lots       = 0.0;
    double      openPrice  = 0.0;
    double      sl         = 0.0;
    double      tp         = 0.0;
    double      profit     = 0.0;  // P&L courant (profit+swap+commission)
    double      swap       = 0.0;
    double      commission = 0.0;
    TimePoint   openTime   = {};
    std::string comment    = {};
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

    // Garde cascade professionnel (3 conditions de qualité marché)
    double      cascadePriceDispatchPct = 0.50; // [1] 50% du GridGap comme déplacement min
    double      cascadeSpreadMaxMult    = 2.5;  // [2] spread max = 2.5× spread moyen EWMA
    double      cascadeSpreadEwmaAlpha  = 0.05; // lissage spread référence EWMA
    // [3] Anti-whipsaw : automatique, pas de paramètre
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

    // ─── Trail Stop adaptatif (4 axes) ──────────────────────
    // [1] Step Trail : seuil minimum de déplacement (% du GridGap)
    //     Ex: 0.15 = le stop ne bouge que si le prix s'est déplacé
    //     de 15% du GridGap depuis le dernier déplacement
    double      trailStepPct        = 0.15;

    // [2] Gel du trail en zone TP : stop figé si basket > target × ratio
    //     Ex: 0.60 = stops figés quand P&L > 60% du target
    double      trailFreezeZone     = 0.60;

    // [3] Gap expansion par niveau de recovery (% par niveau)
    //     Ex: 0.05 = au recovery 10, gap élargi de +50%
    //     Évite la cascade rapide sur les mêmes niveaux de prix
    double      gapExpansionFactor  = 0.05;

    // [4] Trail asymétrique : facteur d'élargissement du côté lourd
    //     Ex: 0.15 → côté lourd : gap × 1.15, côté léger : gap × 0.85
    //     Le côté en deep recovery est plus conservateur
    double      trailAsymmetryFactor = 0.15;

    // ─── 5 fonctionnalités académiques ──────────────────────
    // [A] Gap OU Optimal (Avellaneda-Lee 2010)
    //     Priorité sur ATR si le marché est mean-reverting (ρ₁ < 0)
    //     Fallback automatique sur ATR si trending
    bool        useOUGrid           = false;
    int         ouWindowSize        = 200;   // rolling window estimation OU
    double      ouMinTheta          = 0.05;  // θ minimum pour valider le régime

    // [B] CPPI Lot Recovery (Black-Perold 1992)
    //     Protection naturelle : lot → 0 quand equity → floor
    bool        useCPPI             = false;
    double      cppiMultiplier      = 3.0;   // k : agressivité (2–5)
    double      cppiFloorPct        = 0.80;  // floor = balance × 80%

    // [C] SRH Lot auto-calibrant (Symmetric Recursive Hedge)
    //     lot = drawdown_$ / (gap × tickValue) — récupère en 1 move
    //     Priorité sur CPPI et martingale si activé
    bool        useSRH              = false;
    double      srhMaxLot           = 100.0; // plafond absolu

    // [D] Target VABT dynamique (Risk Parity / Bridgewater)
    //     target = ATR × vabtCoeff × totalLots × tickValue
    bool        useVABT             = false;
    double      vabtCoeff           = 0.15;  // coefficient de calibration

    // [E] Rolling Kelly dynamique (extension de Thorp 1969)
    //     Recalcule f* sur les N derniers cycles réels fermés
    bool        useRollingKelly     = true;  // actif par défaut
    int         kellyHistoryN       = 20;    // N derniers cycles
    double      kellySafetyFactor   = 0.25;  // fraction de f* à utiliser

    // ─── Valeur du tick ($ par lot par point) ───────────────
    //     Requis pour SRH, CPPI et VABT
    //     XAUUSD 5-digit broker : 0.10  (100oz × 0.001 = 0.1$/lot/pt)
    //     EURUSD 5-digit broker : 1.00  (100000 × 0.00001 = 1$/lot/pt)
    //     Mis à jour automatiquement lors de la détection broker
    double      tickValue           = 0.10;

    // ─── Spécifications broker auto-détectées ────────────────
    //     Remplies par detectAndApplyBrokerSpec() au démarrage
    //     → utilisées pour sizing, validation, SL/TP placement
    double      volumeMin           = 0.01;   // lot min broker
    double      volumeMax           = 100.0;  // lot max broker
    double      volumeStep          = 0.01;   // incrément lot broker
    double      stopLevelPoints     = 0.0;    // distance min SL/TP en points
    double      contractSize        = 100.0;  // taille contrat (100oz pour XAU)

    // ─── Basket fermeture contextualisée (profondeur recovery) ──
    // A partir de ce nombre de niveaux combinés SS+BS, le target
    // requis est augmenté de basketRecoveryBonus par niveau supplémentaire
    // Ex: threshold=5, bonus=0.05 → au niveau 7 : target × (1 + 2×0.05) = +10%
    int         basketRecoveryThreshold = 5;
    double      basketRecoveryBonus     = 0.05;

    // ─── Connexion MetaApi ───────────────────────────────────
    // token et accountId lus depuis METAAPI_TOKEN / METAAPI_ACCOUNT_ID
    // region lue depuis METAAPI_REGION (défaut : new-york)
    int         tickPollMs      = 200;   // fréquence polling tick (ms)
    int         statePollMs     = 2000;  // fréquence polling compte+ordres (ms)
    int         reconnectMs     = 1000;  // délai de reconnexion (ms)
};

// ─── Résultat d'une opération de trading ────────────────────
struct TradeResult {
    bool    success  = false;
    int     ticket   = -1;
    int     error    = 0;
    double  price    = 0.0;
    std::string msg  = {};
};

// ============================================================
//  MULTI-AGENT — Enums étendus
// ============================================================

// ─── Décision du Visual AI (assistante marché temps réel) ───
enum class MarketDecision : uint8_t {
    CONTINUE = 0,       // Activité normale — rien à modifier
    TRAIL_AGGRESSIVE,   // Trailing serré — réactivité maximale
    TRAIL_NATURAL,      // Trailing paramètres normaux
    TRAIL_FREEZE,       // Geler les stops — zone TP proche
    ADD_ORDER,          // Ouvrir position supplémentaire
    REMOVE_TRAIL,       // Supprimer le trailing stop
    NO_TRADE,           // Aucun nouveau trade
    OBSERVE,            // Observer niveau/structure sans agir
    LOCK_WAIT,          // Verrouiller — oscillation serrée/piège
    STOP_ORDERS,        // Stopper tous les ordres
};

// ─── Mode trail appliqué par chaque sous-agent ──────────────
enum class TrailMode : uint8_t {
    NATURAL = 0,        // Trail normal (step_pct conf)
    AGGRESSIVE,         // Trail serré (×0.5 du step normal)
    FROZEN,             // Trail figé — stops immobiles
    REMOVED,            // Trail supprimé — stop au prix actuel
};

// ─── État d'un slot de grille (sous-agent niveau Ln) ────────
enum class SlotState : uint8_t {
    IDLE = 0,           // Niveau libre
    ARMED,              // SS + BS placés, attente déclenchement
    TRIGGERED_SS,       // SellStop déclenché
    TRIGGERED_BS,       // BuyStop déclenché
    DUAL_ACTIVE,        // SS et BS tous les deux actifs
    CLOSING,            // Fermeture en cours
};

// ─── Structure de marché détectée par le Visual AI ──────────
enum class MarketStructure : uint8_t {
    UNKNOWN = 0,
    BULLISH_TREND,      // HH + HL confirmés
    BEARISH_TREND,      // LH + LL confirmés
    RANGE_TIGHT,        // Oscillation serrée < 0.5×ATR
    RANGE_WIDE,         // Range large propice au grid
    BREAKOUT_UP,        // Cassure haussière en cours
    BREAKOUT_DOWN,      // Cassure baissière en cours
    TRAP_BULL,          // Faux cassure haussière (piège)
    TRAP_BEAR,          // Faux cassure baissière (piège)
};

// ─── Session de trading ──────────────────────────────────────
enum class TradingSession : uint8_t {
    CLOSED = 0,         // Week-end / hors marché
    TOKYO,              // 00:00–09:00 UTC   (liquidité faible)
    LONDON,             // 08:00–17:00 UTC   (liquidité élevée)
    NEW_YORK,           // 13:00–22:00 UTC   (liquidité élevée)
    OVERLAP_LN,         // 13:00–17:00 UTC   (pic de liquidité)
};

} // namespace top4ea
