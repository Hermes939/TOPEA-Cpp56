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

    // ─── Lots recovery ──────────────────────────────────────
    // useFixedLotSchedule=true  → schedule fixe [0.10, 0.10, 0.20, 0.30, 0.40]
    // useFixedLotSchedule=false + lotMultiplierMax > lotMultiplier
    //                           → gradient x1.15 à x1.66 selon profondeur
    // useFixedLotSchedule=false + lotMultiplierMax == lotMultiplier
    //                           → géométrique baseLot × mult^n (Kelly-cappé)
    [[nodiscard]] double sellLot() const noexcept {
        if (cfg_.useFixedLotSchedule)
            return math::recovLotFixed(cfg_.baseLot, sellCount_.load());
        if (cfg_.lotMultiplierMax > cfg_.lotMultiplier)
            return math::recovLotGradient(cfg_.baseLot,
                                          cfg_.lotMultiplier,
                                          cfg_.lotMultiplierMax,
                                          sellCount_.load(),
                                          cfg_.maxRecovery,
                                          cfg_.kellyMaxFraction);
        return math::recovLot(cfg_.baseLot, cfg_.lotMultiplier,
                              sellCount_.load(), cfg_.kellyMaxFraction);
    }
    [[nodiscard]] double buyLot() const noexcept {
        if (cfg_.useFixedLotSchedule)
            return math::recovLotFixed(cfg_.baseLot, buyCount_.load());
        if (cfg_.lotMultiplierMax > cfg_.lotMultiplier)
            return math::recovLotGradient(cfg_.baseLot,
                                          cfg_.lotMultiplier,
                                          cfg_.lotMultiplierMax,
                                          buyCount_.load(),
                                          cfg_.maxRecovery,
                                          cfg_.kellyMaxFraction);
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

    // ─── Statut live pour affichage (données du tick courant) ─
    // Utilise les données tick les plus récentes de l'EWMA spread
    // et la dernière position de déclenchement pour calculer un
    // résultat frais sans attendre un trigger de recovery.
    [[nodiscard]] CascadeCheckResult liveCheck(double gridGap) const noexcept {
        std::lock_guard lk(mu_);
        return guard_.check(lastSide_, lastTriggerPrice_, gridGap);
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

    // ─── Incrément forcé ────────────────────────────────────
    // Utilisé quand le broker a exécuté un stop (position réelle ouverte)
    // mais que CascadeGuard a bloqué l'incrément automatique.
    // Garantit que le compteur reflète le nombre réel de niveaux de recovery.
    void forceIncrementSell() noexcept { ++sellCount_; }
    void forceIncrementBuy()  noexcept { ++buyCount_;  }

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
    double              lastTriggerPrice_ = 0.0;  // dernier prix de trigger (pour liveCheck)
    int                 lastSide_         = 0;    // dernier côté déclenché (0=BUY/1=SELL)
    mutable std::mutex  mu_;
};

} // namespace top4ea
