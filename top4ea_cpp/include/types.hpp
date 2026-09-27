#pragma once
// ============================================================
//  TOP4EA — types.hpp
//  Définitions centrales partagées — C++20
//  Compatible MT4 et MT5 via MetaApi REST
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

// ─── Plateforme MetaTrader ───────────────────────────────────
// Auto : détection depuis l'info compte MetaApi au démarrage
// MT4  : forcer comportement MT4 (slippage, pas de fillingMode)
// MT5  : forcer comportement MT5 (deviation, fillingMode, POSITION_MODIFY)
enum class PlatformType : uint8_t {
    Auto = 0,
    MT4  = 4,
    MT5  = 5
};

// ─── Types de marché MT4/MT5 ─────────────────────────────────
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
    Init              = 0,
    GridActive        = 1,
    Recovery          = 2,
    Closing           = 3,
    DailyPause        = 4,
    MaxRecoveryHold   = 5   // maxRecovery atteint — positions gelées, attente retournement
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
    double       balance      = 0.0;
    double       equity       = 0.0;
    double       freeMargin   = 0.0;
    double       marginLevel  = 0.0;
    double       marginReq    = 0.0;     // par lot
    int          leverage     = 0;
    std::string  currency     = {};
    PlatformType platform     = PlatformType::Auto;  // détecté depuis MetaApi
};

// ─── Paramètres configurables (équivalent extern MQL4) ──────
struct Config {
    // ─── Plateforme ──────────────────────────────────────────
    // Auto : détecté depuis le compte MetaApi au démarrage
    // MT4  : forcer MT4 (slippage, pas de fillingMode)
    // MT5  : forcer MT5 (deviation, fillingMode FOK, POSITION_MODIFY)
    PlatformType platform       = PlatformType::Auto;

    std::string symbol          = "XAUUSDm";
    double      baseLot         = 0.10;
    double      lotMultiplier   = 1.15;   // multiplicateur min (recovery 1)
    double      lotMultiplierMax = 1.66;  // multiplicateur max (recovery maxN)
    int         gridGap         = 500;          // points – valeur initiale
    double      basketTarget    = 7.0;          // $ fixe profit target
    double      basketMaxLoss   = -300.0;       // $ seuil d'urgence si maxRecoveryHold (négatif)
    int         maxRecovery     = 20;

    // Garde cascade professionnel (3 conditions de qualité marché)
    double      cascadePriceDispatchPct  = 0.50; // [1] 50% du GridGap comme déplacement min
    int         cascadePriceDispatchPips = 15;   // [1b] déplacement fixe en pips (0=utilise pct)
    double      cascadeSpreadMaxMult     = 2.5;  // [2] spread max = 2.5× spread moyen EWMA
    double      cascadeSpreadEwmaAlpha   = 0.05; // lissage spread référence EWMA
    // [3] Anti-whipsaw : automatique, pas de paramètre
    int         magic           = 10300;
    int         slippage        = 3;
    double      dailyTargetPct  = 10.0;         // 0 = désactivé
    // 0 = AUTO-DÉTECTION broker (défaut). Aucun rescale parasite ne sera appliqué.
    // Les valeurs de grille dans top4ea.conf sont exprimées nativement en digits=3.
    // > 0 = surcharge manuelle ; si ≠ broker → rescale appliqué et WARNING logué.
    int         digits          = 0;
    double      point           = 0.001;        // valeur par défaut digits=3 (Exness XAUUSDm)

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

    // [5] Trail fixe en points (0 = désactivé → utilise demi-GridGap × facteur)
    //     Quand > 0 : SS/BS placés à exactement cette distance du prix courant.
    //     Ex: 100 pts = 10 pips sur XAUUSDm (1 pip = 10 pts / 2 décimales)
    //     Limité par le STOPLEVEL broker (minDistPts) si valeur trop faible.
    int         trailFixedDistPts    = 0;

    // [6] Seuil minimum de déplacement avant de mettre à jour le trail (en points)
    //     0 = réactif à chaque tick (1 pt minimum interne)
    //     Ex: 25 pts = trail recalculé seulement si prix bouge de 25 pts
    //     Avec trailFixedDistPts > 0 : mettre à 1 pour un suivi quasi-continu
    int         trailMinChangePts    = 0;

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
    //     CORRECTION : activé par défaut — le target doit être proportionnel
    //     aux lots engagés, pas fixe à $7 quelle que soit l'exposition.
    bool        useVABT             = true;
    double      vabtCoeff           = 0.40;  // était 0.15 — calibré pour XAUUSDm

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

    // ─── Grille de recovery — schedule de lots fixe ─────────────
    // true  : schedule prédéfini [0.10, 0.10, 0.20, 0.30, 0.40]
    //         L1=entrée 0.10 | L2=hedge 0.10 | L3=renfort 0.20 | L4=0.30 | L5=0.40
    // false : lot géométrique baseLot × lotMultiplier^n avec Kelly cap
    bool        useFixedLotSchedule    = true;

    // ─── TP par vagues (Wave TP) ─────────────────────────────────
    // Fermeture partielle progressive au lieu d'un close tout-ou-rien
    //   Wave 1 : P&L ≥ wave1Target → ferme 50% des positions
    //   Wave 2 : P&L ≥ wave2Target → ferme 30% du restant
    //   Wave 3 : trailing 400 pips sur le reliquat (20%)
    bool        waveEnabled            = true;
    double      wave1Target            = 3.50;  // $ seuil déclenchement vague 1
    double      wave2Target            = 7.00;  // $ seuil déclenchement vague 2

    // ─── Basket fermeture contextualisée (profondeur recovery) ──
    // A partir de ce nombre de niveaux combinés SS+BS, le target
    // requis est augmenté de basketRecoveryBonus par niveau supplémentaire
    // Ex: threshold=5, bonus=0.05 → au niveau 7 : target × (1 + 2×0.05) = +10%
    int         basketRecoveryThreshold = 5;
    double      basketRecoveryBonus     = 0.05;

    // ─── Verrouillage Profit Trailing (V-BS / V-SS) ──────────
    // V-SS (Sell Stop trailing côté BUY) :
    //   S'active quand les Buy sont en profit ≥ vlActivationPts.
    //   Niveau = bid - vlTrailPts. Monte avec le bid (ratchet UP, jamais en baisse).
    //   Déclenchement → toutes les Buy fermées au marché (profit verrouillé).
    //
    // V-BS (Buy Stop trailing côté SELL) :
    //   S'active quand les Sell sont en profit ≥ vlActivationPts.
    //   Niveau = ask + vlTrailPts. Descend avec l'ask (ratchet DOWN, jamais en hausse).
    //   Déclenchement → toutes les Sell fermées au marché (profit verrouillé).
    bool        vlEnabled        = true;
    int         vlActivationPts  = 50;   // profit minimum en pts avant activation du trailing
    int         vlTrailPts       = 30;   // distance trailing en pts (ex: 30 pts = 3 pips XAU)

    // ─── Dual Trail NATURAL / AGGRESSIVE ─────────────────────────
    // Quand trailModeAuto = true, le moteur surveille le profit flottant de la
    // Sell Stop active et bascule en mode AGGRESSIVE quand profit >= xLockPips.
    //
    // NATURAL  : comportement symétrique existant (distances naturalSsDistPts / naturalBsDistPts)
    // AGGRESSIVE : le BS glisse vers ss_entry + bsOvershootPips par steps de aggressiveStepPts/tick
    //   → une fois le verrou atteint, le BS garantit un profit minimum même si déclenché avant target
    //
    // Les 6 paramètres correspondants dans top4ea.conf :
    //   trail_mode_auto, x_lock_pips, bs_overshoot_pips,
    //   aggressive_step_pts, natural_bs_dist_pts, natural_ss_dist_pts
    bool        trailModeAuto      = false; // false = off (comportement actuel conservé)
    // ── Côté SELL (BS AGGRESSIVE) ─────────────────────────────
    int         xLockPips          = 500;   // pts profit SS (Sell) → passage BS AGGRESSIVE
    int         bsOvershootPips    = 500;   // verrou BS = ss_entry - n pts (profit garanti)
    int         aggressiveStepPts  = 200;   // glissement BS par tick en mode AGGRESSIVE
    int         naturalBsDistPts   = 600;   // distance BS NATURAL (0 = trailFixedDistPts)
    int         naturalSsDistPts   = 600;   // distance SS NATURAL (0 = trailFixedDistPts)
    // ── Côté BUY (SS AGGRESSIVE miroir) ──────────────────────
    // Quand le profit flottant des positions BUY >= xLockPipsBuy, la SS glisse
    // progressivement vers bsAvgEntry + ssOvershootPips (AU-DESSUS de l'entrée BUY)
    // garantissant un profit minimum de ssOvershootPips pts au déclenchement SS.
    int         xLockPipsBuy       = 500;   // pts profit BUY → passage SS AGGRESSIVE
    int         ssOvershootPips    = 500;   // verrou SS = bs_entry + n pts (profit garanti BUY)
    int         aggressiveSsStepPts = 200;  // glissement SS par tick en mode AGGRESSIVE BUY

    // ─── Connexion MetaApi ───────────────────────────────────
    // token et accountId lus depuis METAAPI_TOKEN / METAAPI_ACCOUNT_ID
    // region lue depuis METAAPI_REGION (défaut : new-york)
    int         tickPollMs      = 200;   // fréquence polling tick (ms)
    int         statePollMs     = 2000;  // fréquence polling compte+ordres (ms)
    int         reconnectMs     = 1000;  // délai de reconnexion (ms)

    // ─── DeepSeek anti-biais statique ────────────────────────
    // Durée absolue (secondes) après laquelle un signal identique répétitif
    // est considéré périmé, indépendamment du TTL de 45s.
    // 0 = désactivé. Défaut : 300s (5 min).
    int         dsStaleHardSec  = 300;

    // ─── DeepSeek timeout de démarrage ───────────────────────
    // Si DeepSeek a une clé API valide mais n'a produit aucun signal
    // après deepseekTimeoutSec secondes, le moteur bascule en mode local
    // (décisions basées uniquement sur l'analyse locale iniMTA).
    // Le client DeepSeek continue en arrière-plan et reprend dès qu'il répond.
    // 0 = désactivé (attente infinie). Défaut : 120s.
    int         deepseekTimeoutSec = 120;

    // ─── Vote board gate — seuils configurables ──────────────
    // voteGateMinScore    : score absolu minimum pour activer bsOnly/ssOnly
    //                       Ex : 2 → bsOnly si score ≥+2, ssOnly si score ≤-2
    //                       Augmenter (ex: 3) = gate plus strict (moins de blocages)
    //                       Diminuer (ex: 1) = gate plus sensible (plus de blocages)
    // voteGateDsConfHigh  : confidence DeepSeek (%) pour poids ±2 dans le vote
    //                       ET pour le DS gate override sur le vote board
    //                       Défaut : 60. (ex: 70 = DS doit être très confiant)
    // voteGateDsConfLow   : confidence DeepSeek (%) pour poids ±1 dans le vote
    //                       Défaut : 40. Doit être < voteGateDsConfHigh.
    int    voteGateMinScore   = 2;    // seuil ±N pour bsOnly/ssOnly [1–5]
    int    voteGateDsConfHigh = 60;   // conf DS% pour poids ±2 / DS gate override
    int    voteGateDsConfLow  = 40;   // conf DS% pour poids ±1

    // ─── Plafonds de sécurité recovery ───────────────────────
    // maxLots          : plafond absolu de lots total du panier (0 = désactivé)
    //                    Bloque tout nouveau stop si totalLots(orders) >= maxLots
    //                    Sécurité anti-Martingale runaway indépendante du lot/niveau.
    // maxRecoveryActive: nombre maximum de levels de recovery SIMULTANÉMENT actifs
    //                    (0 = désactivé = identique à maxRecovery)
    //                    Ex: maxRecovery=10 mais maxRecoveryActive=3 → au plus 3 niveaux actifs
    double      maxLots             = 0.0;  // 0 = désactivé
    int         maxRecoveryActive   = 0;    // 0 = désactivé

    // ─── BiLSTM Agent — apprentissage continu online ─────────
    // lstm_enabled      : active le BiLSTM (false par défaut — opt-in)
    // lstm_seq_len      : longueur de la fenêtre séquentielle (timesteps)
    // lstm_hidden       : dimension hidden state par direction (×2 avec Bi)
    // lstm_layers       : nombre de couches BiLSTM empilées
    // lstm_heads        : nombre de têtes Multi-Head Attention
    // lstm_lr           : learning rate Adam du réseau en ligne
    // lstm_update_every : ticks entre deux mises à jour gradient
    // lstm_target_update_freq : pas de gradient entre deux syncs target-net
    // lstm_checkpoint_every   : transitions entre deux sauvegardes poids
    // lstm_weights_path       : fichier .bin des poids (load/save)
    // lstm_use_dedicated_binary : si true, IPC via fichiers JSON
    //                             si false, agent embarqué (inline)
    bool        lstmEnabled              = false;
    int         lstmSeqLen               = 32;
    int         lstmHidden               = 64;
    int         lstmLayers               = 2;
    int         lstmHeads                = 4;
    float       lstmLr                   = 1e-4f;
    int         lstmUpdateEvery          = 10;
    int         lstmTargetUpdateFreq     = 50;
    int         lstmCheckpointEvery      = 200;
    std::string lstmWeightsPath          = "automl/output/lstm_weights.bin";
    bool        lstmUseDedicatedBinary   = false;
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
