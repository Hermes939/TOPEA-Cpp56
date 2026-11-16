#include "../include/tickerall_bridge.hpp"

#include <cassert>
#include <iostream>

int main() {
    top4ea::Config cfg;
    cfg.symbol = "XAUUSD.c";
    cfg.tickerallServer = "justMarket-Demo3";
    top4ea::TickerallBridge bridge(cfg);

    bridge.start();
    assert(!bridge.isConnected());

    top4ea::TradeCmd cmd;
    cmd.op = top4ea::TradeCmd::Op::MarketOrder;
    cmd.type = top4ea::OrderType::Buy;
    cmd.lots = 0.01;
    const auto result = bridge.sendCommand(cmd);
    assert(!result.success);

    bridge.stop();
    std::cout << "TickerAll missing credentials fail closed: OK\n";
}
