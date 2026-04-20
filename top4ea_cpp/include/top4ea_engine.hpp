#pragma once
// ============================================================
//  TOP4EA — top4ea_engine.hpp  C++20
//  Moteur principal — orchestre grille, recovery, basket
//  Machine à états centrale
// ============================================================
#include "types.hpp"
#include "metaapi_bridge.hpp"
#include "grid_engine.hpp"
#include "basket_manager.hpp"
#include "recovery_engine.hpp"
#include "intelligence_engine.hpp"
#include "market_intelligence.hpp"
#include "agent_orchestrator.hpp"
#include "profit_lock_manager.hpp"
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

namespace top4ea {

// ============================================================
//  TOP4EA_Engine — coordinateur principal
// ============================================================
class TOP4EA_Engine {
public:
    explicit TOP4EA_Engine(Config cfg);
    ~TOP4EA_Engine();

    // ─── Cycle de vie ────────────────────────────────────────
    void start();
    void stop();

    // ─── API DLL publique ────────────────────────────────────
    [[nodiscard]] bool   isConnected() const noexcept;

    // Injection de tick en mode DLL (MT4 → DLL sans MetaApi)
    void injectTick(const Tick& t);

    // Envoi direct d'une commande trade
    [[nodiscard]] BridgeResponse sendTrade(const TradeCmd& cmd);

    // Fermeture globale de tous les ordres (urgence / fin session)
    // Parallèle : stops supprimés + positions fermées simultanément
    void closeAllOrders();

    // Getters compte
    [[nodiscard]] double getBalance    () const noexcept;
    [[nodiscard]] double getEquity     () const noexcept;
    [[nodiscard]] double getFreeMargin () const noexcept;
    [[nodiscard]] double getDailyPnL   () const noexcept;
    [[nodiscard]] int    getOrderCount () const noexcept;

    // ─── Dashboard / état ────────────────────────────────────
    [[nodiscard]] std::string dashboard() const;
    [[nodiscard]] EAState     state()    const noexcept { return state_.load(); }

private:
    // ─── Callbacks bridge ───────────────────────────────────
    void onTick   (const Tick&            tick);
    void onOrder  (const OrderInfo&       ord);
    void onAccount(const AccountSnapshot& acc);
    void onError  (int code, const std::string& msg);
    void onConnect(bool connected);

    // ─── Auto-détection broker ───────────────────────────────
    // Interroge MetaApi avant le démarrage des threads de polling.
    // Si la détection réussit → applique digits/point/tickValue à cfg_
    //   et recrée tous les sous-systèmes avec la config corrigée.
    // Si elle échoue (weekend, hors-ligne) → conserve config fichier + warning.
    // Appelé dans start() avant bridge_->start() (HTTP synchrone brut).
    void detectAndApplyBrokerSpec();
    void applySymbolSpec(const SymbolSpec& spec);
    void rebuildSubsystems();                  // recrée grid/basket/recovery/intel

    // ─── Logique principale ──────────────────────────────────
    void processTick(const Tick& tick);
    void checkDailyTarget();
    void resetDailyIfNeeded();
    void initOrRestore();
    void restoreState(const std::vector<OrderInfo>& orders);

    // Détection géométrique pure — appelée à CHAQUE tick, sans gate temporelle
    // Le prix franchit un niveau de grille → recovery déclenché immédiatement
    void detectGeometricTriggers(const Tick& tick);

    // Synchronisation fallback — vérifie cohérence tickets avec les ordres réels
    void syncTriggersFallback(const std::vector<OrderInfo>& orders);

    void trailGrid();

    void ensureSellStop(double lot, const std::string& comment,
                        int recovDepth, double gapFactor, bool trailFrozen);
    void ensureBuyStop (double lot, const std::string& comment,
                        int recovDepth, double gapFactor, bool trailFrozen);

    void closeBasketAtomic();
    void closeWavePartial(int wave);  // wave 0=Wave1(50%) wave 1=Wave2(30%)
    void closeOneSide(int sideType);
    void emergencyCloseAll(double currentPnL); // urgence MaxRecoveryHold si basket_max_loss atteint

    // ─── Verrouillage Profit Trailing (V-SS / V-BS) ─────────
    // Met à jour les ordres V-BS et V-SS sur le broker
    // d'après la composition et le tick courant (ratchet trailing).
    void updateProfitLockOrders(const std::vector<OrderInfo>& orders,
                                const Tick& tick);

    // Ferme un côté au marché quand son niveau V est atteint
    // side=0 → ferme positions Buy (V-SS déclenché, profit verrouillé)
    // side=1 → ferme positions Sell (V-BS déclenché, profit verrouillé)
    void closeSideAtVL(int side);

    [[nodiscard]] std::vector<OrderInfo> fetchOrders();
    [[nodiscard]] double basketProfit(
        const std::vector<OrderInfo>& orders) const noexcept;
    [[nodiscard]] int    countMarketOrders(
        const std::vector<OrderInfo>& orders) const noexcept;
    [[nodiscard]] bool   ticketStillPending(
        int ticket, const std::vector<OrderInfo>& orders) const noexcept;

    TradeCmd makeSellStop(double lot, double price, const std::string& comment);
    TradeCmd makeBuyStop (double lot, double price, const std::string& comment);
    TradeCmd makeClose   (const OrderInfo& ord, double price);
    TradeCmd makeCloseBy (int buyTicket, int sellTicket);
    TradeCmd makeDelete  (int ticket);

    [[nodiscard]] double selectLot(int recovCount,
                                   double drawdownUSD,
                                   double equity,
                                   double balance,
                                   double gapPoints) const noexcept;

    [[nodiscard]] double effectiveTarget(
        const std::vector<OrderInfo>& orders) const noexcept;

    [[nodiscard]] double totalLots(
        const std::vector<OrderInfo>& orders) const noexcept;

    void log(const std::string& msg) const;

    // ─── Membres ─────────────────────────────────────────────
    Config                              cfg_;
    SymbolSpec                          symbolSpec_;   // specs broker détectées
    std::unique_ptr<MetaApiBridge>      bridge_;
    std::unique_ptr<GridEngine>         grid_;
    std::unique_ptr<BasketMonitor>      basketMon_;
    std::unique_ptr<RecoveryEngine>     recovery_;
    std::unique_ptr<IntelligenceEngine> intel_;

    // ─── Multi-Agent Layer ───────────────────────────────────
    std::unique_ptr<MarketIntelligence> marketIntel_;
    AgentOrchestrator                   orchestrator_;

    // ─── Verrou Profit (V-SS / V-BS) ────────────────────────
    ProfitLockManager                   vlMgr_;

    // Cache de la dernière analyse (pour dashboard)
    mutable std::mutex                  intelMu_;
    IntelligenceEngine::Analysis        lastIntel_;

    std::atomic<EAState> state_ { EAState::Init };
    mutable std::mutex   mu_;

    std::atomic<int>    ssTicket_ { -1 };
    std::atomic<int>    bsTicket_ { -1 };
    // FIX: Converties en atomic<double> pour éviter les torn read/write
    // entre le thread tick (detectGeometricTriggers) et le thread state
    // (onConnect). std::atomic<double> est garanti lock-free sur x86/x64.
    std::atomic<double> ssLevel_ { 0.0 };
    std::atomic<double> bsLevel_ { 0.0 };

    // ── Horodatages de création SS/BS (anti-doublon) ─────────
    // syncTriggersFallback n'invalide pas un ticket créé depuis
    // moins de kOrderProtectWindowSec secondes (évite faux-positifs
    // dus aux HTTP 429 / latence API).
    TimePoint ssCreatedAt_ = {};
    TimePoint bsCreatedAt_ = {};
    static constexpr int kOrderProtectWindowSec = 10;

    // ── Relance immédiate après trigger géométrique ──────────
    // Quand un SS ou BS se déclenche, ce flag force le passage
    // de la gate HTTP au tick suivant → trailGrid() est appelé
    // sans attendre la seconde réglementaire.
    std::atomic<bool> triggerFired_ { false };

    // ── Ancrage géométrique de la grille ─────────────────────
    // Établi une seule fois au démarrage (ou après basket close)
    // Tous les SS/BS sont placés à origin ± n×GridGap — jamais depuis le prix courant
    double  gridOrigin_  = 0.0;

    std::atomic<int> orderCounter_ { 0 };

    TimePoint lastAction_  = Clock::now();

    double    dailyStartBalance_  = 0.0;
    TimePoint dailyResetTime_     = Clock::now();
    bool      dailyTargetReached_ = false;

    mutable std::mutex accMu_;
    AccountSnapshot    account_;

    mutable std::mutex tickMu_;
    Tick               lastTick_;

    std::jthread      dashThread_;
    std::atomic<bool> running_ { false };

    mutable std::mutex     ordersMu_;
    std::vector<OrderInfo> orders_;

    std::atomic<int>  totalPairs_    { 0 };
    std::atomic<int>  totalSingles_  { 0 };
    std::atomic<bool> basketClosing_ { false };  // garde anti-double close

    // Timeout anti-gel : si CLOSING > 30s sans retour → force reset
    TimePoint         closingStart_  = Clock::now();

    // ─── TP par vagues ───────────────────────────────────────
    // 0 = aucune vague fermée | 1 = Wave1 faite (50%) | 2 = Wave2 faite (+30%)
    std::atomic<int>  waveState_     { 0 };

    // ─── Dual Trail NATURAL / AGGRESSIVE ─────────────────────
    // Mode courant basé sur le profit flottant SS (calculé à chaque tick)
    TrailMode  dualTrailMode_       = TrailMode::NATURAL;
    // Cible progressive du BS en mode AGGRESSIVE (glisse par steps)
    double     aggressiveBsTarget_  = 0.0;
    // Verrou activé : BS a atteint ss_entry + bsOvershootPips
    bool       vLockActive_         = false;
    // Prix du verrou (= ss_entry + bsOvershootPips * point)
    double     vLockPrice_          = 0.0;
    // Dernier profit SS en points (mis à jour dans ensureBuyStop, pour dashboard)
    double     ssProfitPipsCache_   = 0.0;

    // ─── Profit réalisé dans le cycle en cours ───────────────
    // Quand V-SS ou V-BS verrouille et ferme un côté en cours de cycle,
    // le profit encaissé est mémorisé ici.  Le test de target basket
    // additionne : cycleRealizedPnL_ + basketProfit(ordres ouverts).
    // Remis à 0 uniquement dans closeBasketAtomic() (fin de cycle complet).
    double cycleRealizedPnL_ = 0.0;
};

} // namespace top4ea
