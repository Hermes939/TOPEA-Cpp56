// ============================================================
//  TOP4EA — recovery_engine.cpp
//  Machine de recovery avec CascadeGuard professionnel
//  3 conditions : déplacement prix + spread + momentum
// ============================================================
#include "../include/recovery_engine.hpp"
#include <iostream>

namespace top4ea {

RecoveryEngine::RecoveryEngine(const Config& cfg)
    : cfg_(cfg)
    , guard_(CascadeGuard::Params{
        .priceDispatchPct  = cfg.cascadePriceDispatchPct,
        .spreadMaxMult     = cfg.cascadeSpreadMaxMult,
        .momentumTicks     = cfg.cascadeMomentumTicks,
        .spreadEwmaAlpha   = cfg.cascadeSpreadEwmaAlpha
      })
{}

void RecoveryEngine::onTick(const Tick& tick) noexcept {
    guard_.onTick(tick);
}

// ─── SellStop déclenché ─────────────────────────────────────
// direction = -1 : on attend confirmation d'une baisse
TriggerResult RecoveryEngine::onSellStopTriggered(
    double currentBid, double lastStopPrice, int gridGap) noexcept
{
    return checkAndIncrement(sellCount_, 1, -1,
                             lastStopPrice, currentBid, gridGap);
}

// ─── BuyStop déclenché ──────────────────────────────────────
// direction = +1 : on attend confirmation d'une hausse
TriggerResult RecoveryEngine::onBuyStopTriggered(
    double currentBid, double lastStopPrice, int gridGap) noexcept
{
    return checkAndIncrement(buyCount_, 0, +1,
                             lastStopPrice, currentBid, gridGap);
}

TriggerResult RecoveryEngine::checkAndIncrement(
    std::atomic<int>& counter,
    int               sideType,
    int               direction,
    double            lastStopPrice,
    double            currentBid,
    int               gridGap) noexcept
{
    std::lock_guard lk(mu_);

    // ── Vérification cascade professionnelle ─────────────────
    auto check = guard_.check(
        direction,
        lastStopPrice,
        currentBid,
        (double)gridGap,
        cfg_.point,
        guard_.avgSpread()   // spread courant EWMA
    );

    lastCheck_ = check;

    // Log systématique du résultat du garde
    std::cout << "[CascadeGuard] " << check.explain() << "\n";

    if (!check.allowed) {
        std::cout << "[Recovery] BLOQUÉ côté "
                  << (sideType == 0 ? "BUY" : "SELL")
                  << " — conditions marché insuffisantes\n";
        return { TriggerResult::Action::Blocked, sideType,
                 counter.load(), check };
    }

    // ── MaxRecovery guard ────────────────────────────────────
    int cur = counter.load();
    if (cfg_.maxRecovery > 0 && cur >= cfg_.maxRecovery) {
        std::cout << "[Recovery] MaxRecovery atteint côté "
                  << (sideType == 0 ? "BUY" : "SELL") << "\n";
        return { TriggerResult::Action::CloseOneSide, sideType,
                 cur, check };
    }

    // ── Recovery autorisé ────────────────────────────────────
    int next = cur + 1;
    counter.store(next);
    guard_.setLastTriggerPrice(currentBid);

    std::cout << "[Recovery] ✓ AUTORISÉ #" << next
              << " côté " << (sideType == 0 ? "BS" : "SS")
              << " | disp=" << check.priceDisplacement << "pts"
              << " spread=" << check.currentSpread << "pts"
              << " mom=" << check.momentumCount << "ticks\n";

    return { TriggerResult::Action::NewRecovery, sideType,
             next, check };
}

void RecoveryEngine::reset() noexcept {
    sellCount_.store(0);
    buyCount_.store(0);
    guard_.reset();
}

void RecoveryEngine::restoreFromOrders(int sellOrders, int buyOrders) noexcept {
    sellCount_.store(sellOrders);
    buyCount_.store(buyOrders);
    std::cout << "[Recovery] Restauré sellRecov=" << sellOrders
              << " buyRecov=" << buyOrders << "\n";
}

} // namespace top4ea
