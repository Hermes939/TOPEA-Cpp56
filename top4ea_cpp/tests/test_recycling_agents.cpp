#include "../include/recycling_agents.hpp"
#include <cassert>
#include <iostream>

using namespace top4ea;

static Config testConfig() {
    Config cfg;
    cfg.volumeMin = 0.01;
    cfg.volumeStep = 0.01;
    cfg.volumeMax = 100.0;
    cfg.agentXCadenceTicks = 1;
    cfg.agentYCadenceTicks = 1;
    cfg.agentZCadenceTicks = 1;
    return cfg;
}

static OrderInfo position(int ticket, OrderType type, double lots, double pnl) {
    OrderInfo order;
    order.ticket = ticket;
    order.type = type;
    order.lots = lots;
    order.profit = pnl;
    return order;
}

int main() {
    {
        RecyclingAgents agents(testConfig());
        agents.onTick();
        const std::vector<OrderInfo> orders{
            position(101, OrderType::Buy, 0.05, -9.0)
        };
        auto action = agents.step(orders, 1000.0, 0.75, 0, false);
        assert(action);
        assert(action->agent == RecyclingClass::X);
        assert(action->type == RecyclingActionType::CloseFull);
        assert(action->ticket == 101);
        assert(action->closeLots == 0.05);
    }

    {
        RecyclingAgents agents(testConfig());
        agents.onTick();
        const std::vector<OrderInfo> orders{
            position(151, OrderType::Buy, 0.10, -9.0)
        };
        auto action = agents.step(orders, 10000.0, 0.75, 0, false, 100.0);
        assert(!action);
    }

    {
        RecyclingAgents agents(testConfig());
        agents.onTick();
        const std::vector<OrderInfo> orders{
            position(201, OrderType::Buy, 0.10, -60.0),
            position(202, OrderType::Sell, 0.20, 100.0)
        };
        auto action = agents.step(orders, 10000.0, 0.20, -1, true);
        assert(action);
        assert(action->agent == RecyclingClass::Y);
        assert(action->type == RecyclingActionType::PairClose);
        assert(action->ticket == 201);
        assert(action->companionTicket == 202);
        assert(action->companionLots == 0.12);
    }

    {
        RecyclingAgents agents(testConfig());
        agents.onTick();
        const std::vector<OrderInfo> orders{
            position(301, OrderType::Buy, 0.40, -200.0)
        };
        auto action = agents.step(orders, 10000.0, 0.20, 0, false);
        assert(action);
        assert(action->agent == RecyclingClass::Z);
        assert(action->type == RecyclingActionType::ClosePartial);
        assert(action->ticket == 301);
        assert(action->closeLots == 0.20);
    }

    std::cout << "X/Y/Z recycling decisions: OK\n";
}
