// ============================================================
//  TOP4EA — recovery_engine.cpp
//  Machine de recovery avec anti-cascade strict
// ============================================================
#include "../include/recovery_engine.hpp"
#include <iostream>

namespace top4ea {

RecoveryEngine::RecoveryEngine(const Config& cfg) : cfg_(cfg) {}

TriggerResult RecoveryEngine::onSellStopTriggered() noexcept {
    std::lock_guard lk(mu_);
    if (!canTrigger()) {
        std::cout << "[Recovery] Anti-cascade actif SS ("
                  << cooldownRemainingSec() << "s)\n";
        return { TriggerResult::Action::AntiCascade, 1, sellCount_.load() };
    }
    return checkMaxAndIncrement(sellCount_, 1);
}

TriggerResult RecoveryEngine::onBuyStopTriggered() noexcept {
    std::lock_guard lk(mu_);
    if (!canTrigger()) {
        std::cout << "[Recovery] Anti-cascade actif BS ("
                  << cooldownRemainingSec() << "s)\n";
        return { TriggerResult::Action::AntiCascade, 0, buyCount_.load() };
    }
    return checkMaxAndIncrement(buyCount_, 0);
}

TriggerResult RecoveryEngine::checkMaxAndIncrement(
    std::atomic<int>& counter, int sideType) noexcept
{
    int cur = counter.load();
    if (cfg_.maxRecovery > 0 && cur >= cfg_.maxRecovery) {
        std::cout << "[Recovery] MaxRecovery atteint côté "
                  << (sideType == 0 ? "BUY" : "SELL") << "\n";
        return { TriggerResult::Action::CloseOneSide, sideType, cur };
    }
    int next = cur + 1;
    counter.store(next);
    lastTrigger_ = Clock::now();
    std::cout << "[Recovery] DÉCLENCHÉ #" << next
              << " côté " << (sideType == 0 ? "BS" : "SS") << "\n";
    return { TriggerResult::Action::NewRecovery, sideType, next };
}

void RecoveryEngine::reset() noexcept {
    sellCount_.store(0);
    buyCount_.store(0);
    lastTrigger_ = Clock::now();
}

void RecoveryEngine::restoreFromOrders(int sellOrders, int buyOrders) noexcept {
    sellCount_.store(sellOrders);
    buyCount_.store(buyOrders);
    std::cout << "[Recovery] Restauré sellRecov=" << sellOrders
              << " buyRecov=" << buyOrders << "\n";
}

} // namespace top4ea
