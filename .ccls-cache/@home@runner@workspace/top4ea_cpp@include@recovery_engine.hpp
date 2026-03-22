#pragma once
// ============================================================
//  TOP4EA — recovery_engine.hpp
//  Contrôle absolu de chaque cycle de recovery
//  Anti-cascade, suivi compteur SS/BS, MaxRecovery guard
// ============================================================
#include "types.hpp"
#include "math_core.hpp"
#include <chrono>
#include <atomic>
#include <mutex>

namespace top4ea {

// ─── Résultat d'une analyse de déclenchement ────────────────
struct TriggerResult {
    enum class Action : uint8_t {
        None,           // pas de déclenchement
        NewRecovery,    // incrément recovery + relance grille
        CloseOneSide,   // MaxRecovery atteint → fermer côté
        AntiCascade     // cooldown actif, ignorer
    };
    Action action   = Action::None;
    int    side     = 0;   // OP_BUY=0 / OP_SELL=1
    int    count    = 0;   // nouveau compteur recovery
};

// ============================================================
//  RecoveryEngine — machine à états pour les recoveries
// ============================================================
class RecoveryEngine {
public:
    explicit RecoveryEngine(const Config& cfg);

    // ─── Déclenchements stops ───────────────────────────────
    // Appeler quand un stop pendand a disparu (déclenché)
    TriggerResult onSellStopTriggered() noexcept;
    TriggerResult onBuyStopTriggered()  noexcept;

    // ─── Lot pour le prochain recovery ──────────────────────
    [[nodiscard]] double sellLot() const noexcept {
        return math::recovLot(cfg_.baseLot, cfg_.lotMultiplier,
                              sellCount_.load(), cfg_.kellyMaxFraction); }

    [[nodiscard]] double buyLot()  const noexcept {
        return math::recovLot(cfg_.baseLot, cfg_.lotMultiplier,
                              buyCount_.load(), cfg_.kellyMaxFraction); }

    // ─── Accès état ─────────────────────────────────────────
    [[nodiscard]] int sellCount() const noexcept { return sellCount_.load(); }
    [[nodiscard]] int buyCount()  const noexcept { return buyCount_.load();  }

    [[nodiscard]] bool canTrigger() const noexcept {
        using namespace std::chrono;
        auto elapsed = duration_cast<seconds>(Clock::now() - lastTrigger_);
        return elapsed.count() >= cfg_.antiCascadeSec;
    }

    [[nodiscard]] int cooldownRemainingSec() const noexcept {
        using namespace std::chrono;
        auto elapsed = duration_cast<seconds>(Clock::now() - lastTrigger_);
        int  remain  = cfg_.antiCascadeSec - (int)elapsed.count();
        return (remain > 0) ? remain : 0;
    }

    // ─── Reset (après fermeture basket) ─────────────────────
    void reset() noexcept;
    void resetSell() noexcept { sellCount_.store(0); }
    void resetBuy()  noexcept { buyCount_.store(0);  }

    // ─── Restauration depuis ordres ouverts (redémarrage) ───
    void restoreFromOrders(int sellOrders, int buyOrders) noexcept;

private:
    TriggerResult checkMaxAndIncrement(std::atomic<int>& counter,
                                       int sideType) noexcept;

    Config            cfg_;
    std::atomic<int>  sellCount_ { 0 };
    std::atomic<int>  buyCount_  { 0 };
    TimePoint         lastTrigger_ = Clock::now();
    mutable std::mutex mu_;
};

} // namespace top4ea
