#pragma once
// ============================================================
//  TOP4EA — basket_manager.hpp
//  Surveillance et fermeture ultra-rapide du basket global
//  Pipeline 4 phases : purge → netting → singles → safety net
// ============================================================
#include "types.hpp"
#include "math_core.hpp"
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <chrono>
#include <future>

namespace top4ea {

// ─── Résultat de fermeture basket ───────────────────────────
struct BasketCloseResult {
    int    pairsClosedBy  = 0;    // paires CloseBy réussies
    int    singlesClosedOk= 0;    // singles fermés avec succès
    int    residuals      = 0;    // ordres non fermés après safety net
    bool   pendingDeleteFailed = false;
    double realizedPnL    = 0.0;  // P&L capturé
    Ms     elapsed        = {};   // durée totale
};

// Zone de basket selon nombre d'ordres
enum class BasketZone : uint8_t {
    A = 0,  // 2–4   ordres : refresh chaque single
    B = 1,  // 5–15  ordres : refresh tous les 3
    C = 2   // 16+   ordres : refresh tous les 3
};

[[nodiscard]] inline BasketZone zoneOf(int n) noexcept {
    if (n <= 4)  return BasketZone::A;
    if (n <= 15) return BasketZone::B;
    return BasketZone::C;
}

[[nodiscard]] inline int refreshInterval(BasketZone z) noexcept {
    return (z == BasketZone::A) ? 1 : 3;
}

// ============================================================
//  BasketMonitor — surveillance temps réel du P&L global
// ============================================================
class BasketMonitor {
public:
    explicit BasketMonitor(const Config& cfg);

    // Mis à jour à chaque tick avec le P&L courant du basket
    // recoveryDepth  : total SS+BS recovery levels ouverts
    //   → en deep recovery, le target requis augmente proportionnellement
    //   → évite de fermer trop tôt quand la grille est sous tension
    // overrideTarget : si > 0, remplace cfg_.basketTarget comme base du target
    //   → utilisé par le mode dynamique par lots (basket_target = 0)
    //   → passé depuis effectiveTarget() dans processTick()
    // Renvoie true si le seuil de fermeture est atteint
    bool update(double currentPnL,
                const std::vector<OrderInfo>& orders,
                int    recoveryDepth   = 0,
                double overrideTarget  = -1.0) noexcept;

    // Forcer la vérification (thread-safe)
    [[nodiscard]] bool shouldClose() const noexcept { return close_.load(); }

    // Réinitialise après fermeture
    void reset() noexcept;

    // Stats pour dashboard
    [[nodiscard]] double  currentPnL()  const noexcept { return lastPnL_.load(); }
    [[nodiscard]] double  velocity()    const noexcept { return vel_.velocity(); }
    [[nodiscard]] int     orderCount()  const noexcept { return orderCount_.load(); }
    [[nodiscard]] BasketZone zone()     const noexcept {
        return zoneOf(orderCount_.load()); }

private:
    Config               cfg_;
    math::PnLVelocity    vel_;
    std::atomic<double>  lastPnL_     { 0.0 };
    std::atomic<int>     orderCount_  { 0   };
    std::atomic<bool>    close_       { false };
};

// ============================================================
//  BasketCloser — pipeline de fermeture atomique 4 phases
//  Injection de dépendances : fonctions de trading atomiques
// ============================================================
class BasketCloser {
public:
    // Callbacks vers la couche trading réelle
    using CloseOrderFn = std::function<bool(int ticket, double lots,
                                            double price, int slippage)>;
    using CloseByFn    = std::function<bool(int ticketBuy, int ticketSell)>;
    using DeleteFn     = std::function<bool(int ticket)>;
    using RefreshFn    = std::function<Tick()>;   // récupère Bid/Ask frais

    explicit BasketCloser(const Config& cfg,
                          CloseOrderFn  closeOrder,
                          CloseByFn     closeBy,
                          DeleteFn      deletePending,
                          RefreshFn     refresh);

    // Lance la fermeture complète — bloquant jusqu'à résolution
    BasketCloseResult execute(std::vector<OrderInfo> orders,
                              int ssTicket, int bsTicket);

private:
    // Phase 0 : purge des ordres pendants
    bool phase0_purgePending(int ssTicket, int bsTicket);

    // Phase 1 : netting CloseBy (paires de lots identiques)
    int phase1_netting(std::vector<OrderInfo>& buys,
                       std::vector<OrderInfo>& sells,
                       std::vector<bool>&      buyUsed,
                       std::vector<bool>&      sellUsed);

    // Phase 2 : fermeture singles triés P&L ascendant (losers first)
    int phase2_singles(const std::vector<OrderInfo>& buys,
                       const std::vector<OrderInfo>& sells,
                       const std::vector<bool>&      buyUsed,
                       const std::vector<bool>&      sellUsed,
                       BasketZone zone);

    // Phase 3 : safety net — retry ×3 sur résidus
    int phase3_safetyNet();

    // Tri par lots décroissants
    static void sortByLotDesc(std::vector<OrderInfo>& v);
    // Tri par P&L ascendant (losers first)
    static void sortByPnLAsc(std::vector<OrderInfo>& v);

    Config        cfg_;
    CloseOrderFn  closeOrder_;
    CloseByFn     closeBy_;
    DeleteFn      deletePending_;
    RefreshFn     refresh_;

    Tick currentTick_;
};

} // namespace top4ea
