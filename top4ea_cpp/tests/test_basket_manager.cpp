#include "../include/basket_manager.hpp"
#include <cassert>

int main() {
    top4ea::Config cfg;
    top4ea::Tick tick;
    bool deleteSucceeded = false;

    top4ea::BasketCloser closer(
        cfg,
        [](int, double, double, int) { return true; },
        [](int, int) { return true; },
        [&](int) { return deleteSucceeded; },
        [&]() { return tick; });

    top4ea::OrderInfo pending;
    pending.ticket = 42;
    pending.type = top4ea::OrderType::SellStop;
    auto failedDelete = closer.execute({pending}, 42, -1);
    assert(failedDelete.pendingDeleteFailed);

    deleteSucceeded = true;
    auto successfulDelete = closer.execute({pending}, 42, -1);
    assert(!successfulDelete.pendingDeleteFailed);

    top4ea::OrderInfo position;
    position.ticket = 7;
    position.type = top4ea::OrderType::Buy;
    top4ea::BasketCloser failedClose(
        cfg,
        [](int, double, double, int) { return false; },
        [](int, int) { return false; },
        [](int) { return true; },
        [&]() { return tick; });
    auto failedPositionClose = failedClose.execute({position}, -1, -1);
    assert(failedPositionClose.singlesClosedOk == 0);

    return 0;
}
