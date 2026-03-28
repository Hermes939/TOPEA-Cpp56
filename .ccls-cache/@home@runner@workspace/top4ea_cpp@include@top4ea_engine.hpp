#pragma once
// ============================================================
//  TOP4EA — top4ea_engine.hpp
//  Moteur principal — orchestre grille, recovery, basket
//  Machine à états centrale C++20
// ============================================================
#include "types.hpp"
#include "mt4_bridge.hpp"
#include "grid_engine.hpp"
#include "basket_manager.hpp"
#include "recovery_engine.hpp"
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

    // Démarrage / arrêt
    void start();
    void stop();

    // État pour le dashboard
    [[nodiscard]] std::string dashboard() const;
    [[nodiscard]] EAState     state()    const noexcept { return state_.load(); }

private:
    // ─── Callbacks bridge ───────────────────────────────────
    void onTick   (const Tick&            tick);
    void onOrder  (const OrderInfo&       ord);
    void onAccount(const AccountSnapshot& acc);
    void onError  (int code, const std::string& msg);
    void onConnect(bool connected);

    // ─── Logique principale (appelée sur chaque tick) ───────
    void processTick(const Tick& tick);

    // ─── Surveillance basket ─────────────────────────────────
    void checkBasket();

    // ─── Objectif journalier ────────────────────────────────
    void checkDailyTarget();
    void resetDailyIfNeeded();

    // ─── Initialisation / restauration ──────────────────────
    void initOrRestore();
    void restoreState(const std::vector<OrderInfo>& orders);

    // ─── Gestion des déclenchements stops ───────────────────
    void handleTriggers(const std::vector<OrderInfo>& orders);

    // ─── Trail de grille ────────────────────────────────────
    void trailGrid();

    void ensureSellStop(double lot, const std::string& comment,
                        int recovDepth, double gapFactor, bool trailFrozen);
    void ensureBuyStop (double lot, const std::string& comment,
                        int recovDepth, double gapFactor, bool trailFrozen);

    // ─── Fermetures ─────────────────────────────────────────
    void closeBasketAtomic();
    void closeOneSide(int sideType);

    // ─── Cohérence tickets ──────────────────────────────────
    void checkConsistency(const std::vector<OrderInfo>& orders);

    // ─── Requêtes MT4 ───────────────────────────────────────
    [[nodiscard]] std::vector<OrderInfo> fetchOrders();
    [[nodiscard]] double                 basketProfit(
                        const std::vector<OrderInfo>& orders) const noexcept;
    [[nodiscard]] int                    countMarketOrders(
                        const std::vector<OrderInfo>& orders) const noexcept;
    [[nodiscard]] bool                   ticketStillPending(
                        int ticket,
                        const std::vector<OrderInfo>& orders) const noexcept;

    // ─── Fabrication TradeCmd ────────────────────────────────
    TradeCmd makeSellStop(double lot, double price, const std::string& comment);
    TradeCmd makeBuyStop (double lot, double price, const std::string& comment);
    TradeCmd makeClose   (const OrderInfo& ord, double price);
    TradeCmd makeCloseBy (int buyTicket, int sellTicket);
    TradeCmd makeDelete  (int ticket);

    // ─── Sélection du lot (SRH / CPPI / Kelly) ──────────────
    // drawdownUSD : max(0, -BasketProfit) — utilisé par SRH
    // equity/bal  : utilisés par CPPI
    [[nodiscard]] double selectLot(int recovCount,
                                   double drawdownUSD,
                                   double equity,
                                   double balance,
                                   double gapPoints) const noexcept;

    // ─── Target VABT dynamique ───────────────────────────────
    [[nodiscard]] double effectiveTarget(
        const std::vector<OrderInfo>& orders) const noexcept;

    // ─── Total lots des ordres de marché ─────────────────────
    [[nodiscard]] double totalLots(
        const std::vector<OrderInfo>& orders) const noexcept;

    // ─── Log ─────────────────────────────────────────────────
    void log(const std::string& msg) const;

    // ─── Membres ─────────────────────────────────────────────
    Config                     cfg_;
    std::unique_ptr<MT4Bridge> bridge_;
    std::unique_ptr<GridEngine>    grid_;
    std::unique_ptr<BasketMonitor> basketMon_;
    std::unique_ptr<RecoveryEngine> recovery_;

    std::atomic<EAState> state_ { EAState::Init };
    mutable std::mutex   mu_;

    // Tickets des stops pendants
    std::atomic<int> ssTicket_ { -1 };
    std::atomic<int> bsTicket_ { -1 };
    double  ssLevel_  = 0.0;
    double  bsLevel_  = 0.0;

    // Compteur d'ordres
    std::atomic<int> orderCounter_ { 0 };

    // Anti-cascade
    TimePoint lastTrigger_ = Clock::now();
    TimePoint lastAction_  = Clock::now();

    // Objectif journalier
    double    dailyStartBalance_  = 0.0;
    TimePoint dailyResetTime_     = Clock::now();
    bool      dailyTargetReached_ = false;

    // Snapshot compte courant
    mutable std::mutex  accMu_;
    AccountSnapshot     account_;

    // Tick courant
    mutable std::mutex tickMu_;
    Tick               lastTick_;

    // Thread dashboard
    std::jthread dashThread_;
    std::atomic<bool> running_ { false };

    // Cache ordres (rafraîchi par callback onOrder)
    mutable std::mutex ordersMu_;
    std::vector<OrderInfo> orders_;

    // Statistiques
    std::atomic<int>    totalPairs_   { 0 };
    std::atomic<int>    totalSingles_ { 0 };
};

} // namespace top4ea
