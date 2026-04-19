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
    void checkBasket();
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
    void closeOneSide(int sideType);
    void checkConsistency(const std::vector<OrderInfo>& orders);

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

    // Cache de la dernière analyse (pour dashboard)
    mutable std::mutex                  intelMu_;
    IntelligenceEngine::Analysis        lastIntel_;

    std::atomic<EAState> state_ { EAState::Init };
    mutable std::mutex   mu_;

    std::atomic<int> ssTicket_ { -1 };
    std::atomic<int> bsTicket_ { -1 };
    double  ssLevel_     = 0.0;
    double  bsLevel_     = 0.0;

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
};

} // namespace top4ea
