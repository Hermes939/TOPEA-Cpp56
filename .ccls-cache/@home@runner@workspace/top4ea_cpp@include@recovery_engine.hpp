#pragma once
// ============================================================
//  TOP4EA — recovery_engine.hpp
//  Contrôle absolu de chaque cycle de recovery
//  CascadeGuard professionnel : prix + spread + momentum
// ============================================================
#include "types.hpp"
#include "math_core.hpp"
#include "cascade_guard.hpp"
#include <atomic>
#include <mutex>

namespace top4ea {

// ─── Résultat d'une analyse de déclenchement ────────────────
struct TriggerResult {
    enum class Action : uint8_t {
        None,           // pas de déclenchement
        NewRecovery,    // conditions validées → recovery autorisé
        CloseOneSide,   // MaxRecovery atteint → fermer côté
        Blocked         // CascadeGuard a bloqué (avec explication)
    };
    Action            action      = Action::None;
    int               side        = 0;   // 0=BUY / 1=SELL
    int               count       = 0;   // nouveau compteur recovery
    CascadeCheckResult cascadeInfo = {};  // détail des 3 conditions
};

// ============================================================
//  RecoveryEngine — machine à états pour les recoveries
// ============================================================
class RecoveryEngine {
public:
    explicit RecoveryEngine(const Config& cfg);

    // ─── Alimentation tick (requis pour le CascadeGuard) ────
    void onTick(const Tick& tick) noexcept;

    // ─── Déclenchements stops ───────────────────────────────
    // currentBid : prix courant au moment du déclenchement
    // gridGap    : GridGap effectif courant (ATR ou fixe)
    TriggerResult onSellStopTriggered(double currentBid,
                                      double lastStopPrice,
                                      int    gridGap) noexcept;

    TriggerResult onBuyStopTriggered (double currentBid,
                                      double lastStopPrice,
                                      int    gridGap) noexcept;

    // ─── Lot pour le prochain recovery ──────────────────────
    [[nodiscard]] double sellLot() const noexcept {
        return math::recovLot(cfg_.baseLot, cfg_.lotMultiplier,
                              sellCount_.load(), cfg_.kellyMaxFraction); }

    [[nodiscard]] double buyLot() const noexcept {
        return math::recovLot(cfg_.baseLot, cfg_.lotMultiplier,
                              buyCount_.load(), cfg_.kellyMaxFraction); }

    // ─── Accès état ─────────────────────────────────────────
    [[nodiscard]] int sellCount() const noexcept { return sellCount_.load(); }
    [[nodiscard]] int buyCount()  const noexcept { return buyCount_.load();  }

    // Info cascade pour dashboard
    [[nodiscard]] CascadeCheckResult lastCascadeCheck() const noexcept {
        std::lock_guard lk(mu_);
        return lastCheck_;
    }

    // ─── Reset ──────────────────────────────────────────────
    void reset()     noexcept;
    void resetSell() noexcept { sellCount_.store(0); }
    void resetBuy()  noexcept { buyCount_.store(0);  }

    // ─── Restauration depuis ordres ouverts (redémarrage) ───
    void restoreFromOrders(int sellOrders, int buyOrders) noexcept;

private:
    TriggerResult checkAndIncrement(std::atomic<int>& counter,
                                    int               sideType,
                                    int               direction,
                                    double            lastStopPrice,
                                    double            currentBid,
                                    int               gridGap) noexcept;

    Config              cfg_;
    CascadeGuard        guard_;
    std::atomic<int>    sellCount_ { 0 };
    std::atomic<int>    buyCount_  { 0 };
    CascadeCheckResult  lastCheck_;
    mutable std::mutex  mu_;
};

} // namespace top4ea
