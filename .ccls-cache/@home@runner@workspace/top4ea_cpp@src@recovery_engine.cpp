// ============================================================
//  TOP4EA — recovery_engine.cpp
//  CascadeGuard : déplacement prix + spread + anti-whipsaw
//  Stratégie hedging pur — zéro prédiction de direction
// ============================================================
#include "../include/recovery_engine.hpp"
#include <iostream>

namespace top4ea {

RecoveryEngine::RecoveryEngine(const Config& cfg)
    : cfg_(cfg)
    , guard_(CascadeGuard::Params{
        .priceDispatchPct = cfg.cascadePriceDispatchPct,
        .spreadMaxMult    = cfg.cascadeSpreadMaxMult,
        .spreadEwmaAlpha  = cfg.cascadeSpreadEwmaAlpha
      })
{}

void RecoveryEngine::onTick(const Tick& tick) noexcept {
    guard_.onTick(tick, cfg_.point);
}

// ─── SellStop déclenché ─────────────────────────────────────
// side = 1 (SELL) : le prix a descendu sous triggerPrice
// Anti-whipsaw : le prix doit rester sous triggerPrice
TriggerResult RecoveryEngine::onSellStopTriggered(
    double triggerPrice, int gridGap) noexcept
{
    return checkAndIncrement(sellCount_, 1, triggerPrice, gridGap);
}

// ─── BuyStop déclenché ──────────────────────────────────────
// side = 0 (BUY) : le prix a monté au-dessus de triggerPrice
// Anti-whipsaw : le prix doit rester au-dessus de triggerPrice
TriggerResult RecoveryEngine::onBuyStopTriggered(
    double triggerPrice, int gridGap) noexcept
{
    return checkAndIncrement(buyCount_, 0, triggerPrice, gridGap);
}

TriggerResult RecoveryEngine::checkAndIncrement(
    std::atomic<int>& counter,
    int    side,
    double triggerPrice,
    int    gridGap) noexcept
{
    std::lock_guard lk(mu_);

    // ── 3 conditions de qualité marché ───────────────────────
    auto chk = guard_.check(side, triggerPrice, (double)gridGap);
    lastCheck_ = chk;

    std::cout << "[CascadeGuard] " << chk.explain() << "\n";

    if (!chk.allowed) {
        std::cout << "[Recovery] BLOQUÉ côté "
                  << (side == 0 ? "BUY" : "SELL")
                  << " — qualité marché insuffisante\n";
        return { TriggerResult::Action::Blocked, side,
                 counter.load(), chk };
    }

    // ── MaxRecovery ──────────────────────────────────────────
    int cur = counter.load();
    if (cfg_.maxRecovery > 0 && cur >= cfg_.maxRecovery) {
        std::cout << "[Recovery] MaxRecovery atteint côté "
                  << (side == 0 ? "BUY" : "SELL") << "\n";
        return { TriggerResult::Action::CloseOneSide, side, cur, chk };
    }

    // ── Recovery autorisé ────────────────────────────────────
    int next = cur + 1;
    counter.store(next);
    guard_.recordTrigger(triggerPrice);

    std::cout << "[Recovery] ✓ #" << next
              << " côté " << (side == 0 ? "BS" : "SS")
              << " | disp=" << chk.priceDisplacement << "pts"
              << " | spread=" << chk.currentSpreadPts << "pts"
              << " | whipsaw=non\n";

    return { TriggerResult::Action::NewRecovery, side, next, chk };
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
