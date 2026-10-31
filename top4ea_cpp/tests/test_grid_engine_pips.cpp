#include "../include/grid_engine.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace top4ea;

static bool near(double a, double b, double eps = 1e-8) {
    return std::abs(a - b) <= eps;
}

int main() {
    Config cfg;
    cfg.symbol = "XAUUSD.c";
    cfg.digits = 2;
    cfg.point = 0.01;
    cfg.useAtrGrid = false;
    cfg.gridGap = 200;
    cfg.trailMinChangePips = 0.5;

    assert(near(pricePerPip(cfg), 0.10));

    GridEngine grid(cfg);
    Tick first;
    first.bid = 4255.60;
    first.ask = 4255.86;
    first.spread = first.ask - first.bid;
    grid.onTick(first);

    const double stopDistance = 18.0 * pricePerPip(cfg); // $1.80
    const auto ssCreate = grid.computeSellStop(
        -1, 0.0, 0.0, 0, 1.0, stopDistance);
    const auto bsCreate = grid.computeBuyStop(
        -1, 0.0, 0.0, 0, 1.0, stopDistance);
    assert(ssCreate.needsCreate && near(ssCreate.targetPrice, 4253.80));
    assert(bsCreate.needsCreate && near(bsCreate.targetPrice, 4257.66));

    // A favorable market move updates the corresponding pending stop once the
    // configured half-pip threshold is exceeded.
    Tick moved;
    moved.bid = 4255.80;
    moved.ask = 4256.06;
    moved.spread = moved.ask - moved.bid;
    grid.onTick(moved);
    const auto ssMove = grid.computeSellStop(
        1, ssCreate.targetPrice, 0.0, 0, 1.0, stopDistance);
    assert(ssMove.needsModify && near(ssMove.targetPrice, 4254.00));

    Tick retraced;
    retraced.bid = 4255.40;
    retraced.ask = 4255.66;
    retraced.spread = retraced.ask - retraced.bid;
    grid.onTick(retraced);
    const auto bsMove = grid.computeBuyStop(
        1, bsCreate.targetPrice, 0.0, 0, 1.0, stopDistance);
    assert(bsMove.needsModify && near(bsMove.targetPrice, 4257.46));

    std::cout << "XAUUSD.c pip distances and stop ratchets: OK\n";
}
