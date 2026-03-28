#pragma once
// ============================================================
//  TOP4EA — recovery_engine.hpp
//  Contrôle absolu de chaque cycle de recovery
//  CascadeGuard : déplacement prix + spread + anti-whipsaw
//  Aucune prédiction de direction — hedging pur
// ============================================================
#include "types.hpp"
#include "math_core.hpp"
#include "cascade_guard.hpp"
#include <atomic>
#include <mutex>

namespace top4ea {

// ─── Résultat d'un déclenchement ────────────────────────────
struct TriggerResult {
    enum class Action : uint8_t {
        None,           // rien
        NewRecovery,    // 3 conditions validées → recovery ouvert
        CloseOneSide,   // MaxRecovery atteint → fermer le côté
        Blocked         // CascadeGuard a bloqué (qualité marché insuffisante)
    };
    Action             action      = Action::None;
    int                side        = 0;   // 0=BUY / 1=SELL
    int                count       = 0;   // compteur recovery après incrément
    CascadeCheckResult cascadeInfo = {};  // détail des 3 conditions
};

// ============================================================
//  RecoveryEngine
// ============================================================
class RecoveryEngine {
public:
    explicit RecoveryEngine(const Config& cfg);

    // ─── Alimentation tick (spread EWMA pour le garde) ──────
    void onTick(const Tick& tick) noexcept;

    // ─── Déclenchements stops ───────────────────────────────
    // triggerPrice : prix exact du stop déclenché (openPrice)
    // gridGap      : GridGap effectif courant en points
    TriggerResult onSellStopTriggered(double triggerPrice, int gridGap) noexcept;
    TriggerResult onBuyStopTriggered (double triggerPrice, int gridGap) noexcept;

    // ─── Lots recovery (Kelly-cappés) ───────────────────────
    [[nodiscard]] double sellLot() const noexcept {
        return math::recovLot(cfg_.baseLot, cfg_.lotMultiplier,
                              sellCount_.load(), cfg_.kellyMaxFraction);
    }
    [[nodiscard]] double buyLot() const noexcept {
        return math::recovLot(cfg_.baseLot, cfg_.lotMultiplier,
                              buyCount_.load(), cfg_.kellyMaxFraction);
    }

    // ─── État ───────────────────────────────────────────────
    [[nodiscard]] int sellCount() const noexcept { return sellCount_.load(); }
    [[nodiscard]] int buyCount()  const noexcept { return buyCount_.load();  }

    [[nodiscard]] CascadeCheckResult lastCascadeCheck() const noexcept {
        std::lock_guard lk(mu_);
        return lastCheck_;
    }
    // Spread moyen de référence pour affichage
    [[nodiscard]] double avgSpreadPts() const noexcept {
        return guard_.avgSpreadRaw() / cfg_.point;
    }

    // [E] Rolling Kelly — enregistre un cycle fermé
    void recordBasketClose(double realizedPnL) noexcept {
        rollingKelly_.recordCycle(realizedPnL);
    }
    // Fraction Kelly courante (Rolling ou fixe selon config)
    [[nodiscard]] double kellyFraction() const noexcept {
        if (cfg_.useRollingKelly && rollingKelly_.ready())
            return rollingKelly_.fraction();
        return cfg_.kellyMaxFraction;
    }
    [[nodiscard]] int kellyHistoryCount() const noexcept {
        return rollingKelly_.count();
    }

    // ─── Reset ──────────────────────────────────────────────
    void reset()     noexcept;
    void resetSell() noexcept { sellCount_.store(0); }
    void resetBuy()  noexcept { buyCount_.store(0);  }

    void restoreFromOrders(int sellOrders, int buyOrders) noexcept;

private:
    TriggerResult checkAndIncrement(std::atomic<int>& counter,
                                    int    side,
                                    double triggerPrice,
                                    int    gridGap) noexcept;

    Config              cfg_;
    CascadeGuard        guard_;
    math::RollingKelly  rollingKelly_;   // [E] Kelly dynamique glissant
    std::atomic<int>    sellCount_ { 0 };
    std::atomic<int>    buyCount_  { 0 };
    CascadeCheckResult  lastCheck_;
    mutable std::mutex  mu_;
};

} // namespace top4ea
