#include "../include/recycling_agents.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace top4ea {
namespace {

double netPnl(const OrderInfo& order) noexcept {
    return order.profit + order.swap + order.commission;
}

int directionOf(OrderType type) noexcept {
    return type == OrderType::Buy ? 1 : -1;
}

} // namespace

RecyclingClass RecyclingAgents::classify(
    double lots, double pnl, double usdToAccountScale) const noexcept {
    if (cfg_.agentClassByLots) {
        if (lots < cfg_.agentXLotMax) return RecyclingClass::X;
        if (lots < cfg_.agentYLotMax) return RecyclingClass::Y;
        return RecyclingClass::Z;
    }

    const double loss = std::max(0.0, -pnl);
    if (loss < cfg_.agentXUsdMax * usdToAccountScale)
        return RecyclingClass::X;
    if (loss < cfg_.agentYUsdMax * usdToAccountScale)
        return RecyclingClass::Y;
    return RecyclingClass::Z;
}

double RecyclingAgents::capFor(RecyclingClass agent, int direction,
                                double urgency, int flowSign,
                                double usdToAccountScale) const noexcept {
    const double base = (agent == RecyclingClass::X) ? cfg_.agentXCapUsd
                       : (agent == RecyclingClass::Y) ? cfg_.agentYCapUsd
                                                       : cfg_.agentZDefconUsd;
    const double u = std::clamp(urgency, 0.0, 1.0);
    double cap = base * usdToAccountScale * (1.0 - 0.5 * u);
    if (flowSign != 0 && direction == flowSign)
        cap *= cfg_.agentCapVsFlowMult;
    else if (flowSign != 0 && direction == -flowSign)
        cap *= cfg_.agentCapAgainstMult;
    return std::max(0.0, cap);
}

double RecyclingAgents::normalizeLots(double lots) const noexcept {
    const double minLot = std::max(0.0, cfg_.volumeMin);
    const double step = (cfg_.volumeStep > 0.0) ? cfg_.volumeStep
                                                 : (minLot > 0.0 ? minLot : 0.01);
    if (lots <= 0.0 || step <= 0.0) return 0.0;
    if (cfg_.volumeMax > 0.0) lots = std::min(lots, cfg_.volumeMax);

    const double units = std::floor(lots / step + 1e-9);
    const double normalized = units * step;
    if (normalized + 1e-9 < minLot) return 0.0;
    return normalized;
}

bool RecyclingAgents::cadenceDue(int cadence, uint64_t& lastCycle,
                                  double urgency) noexcept {
    if (cadence <= 1) return true;
    const uint64_t cycle = tickCount_.load(std::memory_order_relaxed) /
                           static_cast<uint64_t>(cadence);
    if (urgency >= 0.75) {
        lastCycle = cycle;
        return true;
    }
    if (cycle <= lastCycle) return false;
    lastCycle = cycle;
    return true;
}

void RecyclingAgents::refreshReports(const std::vector<ClassifiedLeg>& legs) {
    for (auto& report : reports_) {
        report.legs = 0;
        report.lots = 0.0;
        report.pnl = 0.0;
        report.worstPnl = 0.0;
        report.worstTicket = -1;
    }

    for (const auto& leg : legs) {
        auto& report = reports_[static_cast<size_t>(leg.agent)];
        ++report.legs;
        report.lots += leg.order->lots;
        report.pnl += leg.netPnl;
        if (leg.netPnl < report.worstPnl) {
            report.worstPnl = leg.netPnl;
            report.worstTicket = leg.order->ticket;
        }
    }
}

const char* RecyclingAgents::className(RecyclingClass agent) noexcept {
    switch (agent) {
        case RecyclingClass::X: return "X-unités";
        case RecyclingClass::Y: return "Y-dizaines";
        case RecyclingClass::Z: return "Z-centaines";
    }
    return "?";
}

std::optional<RecyclingAction> RecyclingAgents::step(
    const std::vector<OrderInfo>& orders,
    double balance,
    double urgency,
    int flowSign,
    bool monoMode,
    double usdToAccountScale) {
    std::lock_guard lock(mutex_);
    if (!cfg_.agentsEnabled) return std::nullopt;
    if (!std::isfinite(usdToAccountScale) || usdToAccountScale <= 0.0)
        usdToAccountScale = 1.0;
    const auto now = Clock::now();
    if (haveStepped_ && now - lastStepAt_ < std::chrono::seconds(1))
        return std::nullopt;
    lastStepAt_ = now;
    haveStepped_ = true;

    const double u = std::clamp(urgency, 0.0, 1.0);
    flowSign = (flowSign > 0) ? 1 : (flowSign < 0) ? -1 : 0;

    std::vector<ClassifiedLeg> legs;
    legs.reserve(orders.size());
    double totalLots = 0.0;
    double deltaLots = 0.0;
    for (const auto& order : orders) {
        if ((order.type != OrderType::Buy && order.type != OrderType::Sell) ||
            order.ticket <= 0 || order.lots <= 0.0)
            continue;
        const double pnl = netPnl(order);
        const int direction = directionOf(order.type);
        const auto agent = classify(order.lots, pnl, usdToAccountScale);
        legs.push_back({&order, agent, pnl, direction,
                        capFor(agent, direction, u, flowSign,
                               usdToAccountScale)});
        totalLots += order.lots;
        deltaLots += direction * order.lots;
    }
    refreshReports(legs);
    if (legs.empty()) return std::nullopt;

    const bool deadlock = totalLots > 0.0 && std::abs(deltaLots) < 0.01;
    const double safeBalance = std::max(1.0, balance);
    const double sacrificePct = cfg_.agentSacrificePctBase +
        (cfg_.agentSacrificePctEmerg - cfg_.agentSacrificePctBase) * u;
    const double baseBudget = safeBalance * sacrificePct / 100.0;

    auto worstOverCap = [&](RecyclingClass cls) -> const ClassifiedLeg* {
        const ClassifiedLeg* worst = nullptr;
        for (const auto& leg : legs) {
            if (leg.agent != cls || leg.netPnl >= -leg.cap) continue;
            if (!worst || leg.netPnl < worst->netPnl) worst = &leg;
        }
        return worst;
    };

    auto canSacrifice = [&](const ClassifiedLeg& leg) {
        return flowSign == 0 || leg.direction == -flowSign || deadlock;
    };

    auto makeFullClose = [&](const ClassifiedLeg& leg, RecyclingClass cls,
                             const std::string& reason) {
        RecyclingAction action;
        action.type = RecyclingActionType::CloseFull;
        action.agent = cls;
        action.ticket = leg.order->ticket;
        action.closeLots = leg.order->lots;
        action.estimatedPnl = leg.netPnl;
        action.reason = reason;
        return action;
    };

    // AGENT X — threshold control and harvesting of small floating gains.
    if (cadenceDue(cfg_.agentXCadenceTicks, lastXCycle_, u)) {
        if (const auto* worst = worstOverCap(RecyclingClass::X)) {
            const double loss = -worst->netPnl;
            if (canSacrifice(*worst) && (loss <= baseBudget || u >= 0.75))
                return makeFullClose(*worst, RecyclingClass::X,
                                     "sacrifice anti-dérive");
        }

        if (!monoMode) {
            const ClassifiedLeg* best = nullptr;
            const double harvestMax = std::max(
                cfg_.agentHarvestUsd * 2.0, cfg_.agentXCapUsd) *
                usdToAccountScale;
            for (const auto& leg : legs) {
                if (leg.agent != RecyclingClass::X ||
                    leg.netPnl < cfg_.agentHarvestUsd * usdToAccountScale ||
                    leg.netPnl > harvestMax)
                    continue;
                if (!best || leg.netPnl > best->netPnl) best = &leg;
            }
            if (best)
                return makeFullClose(*best, RecyclingClass::X,
                                     "récolte petit gain");
        }
    }

    // AGENT Y — first neutralize a medium loser with an opposing winner.
    if (cadenceDue(cfg_.agentYCadenceTicks, lastYCycle_, u)) {
        if (const auto* loser = worstOverCap(RecyclingClass::Y)) {
            const double loss = -loser->netPnl;
            const ClassifiedLeg* winner = nullptr;
            double bestScore = std::numeric_limits<double>::max();
            for (const auto& leg : legs) {
                if (leg.order->ticket == loser->order->ticket ||
                    leg.direction == loser->direction ||
                    leg.netPnl < loss)
                    continue;
                const double score = std::abs(leg.order->lots - loser->order->lots);
                if (score < bestScore) {
                    bestScore = score;
                    winner = &leg;
                }
            }

            if (winner && winner->order->lots > 0.0) {
                const double fraction = std::min(1.0, loss / winner->netPnl);
                const double closeLots = normalizeLots(winner->order->lots * fraction);
                if (closeLots > 0.0 && closeLots <= winner->order->lots) {
                    RecyclingAction action;
                    action.type = RecyclingActionType::PairClose;
                    action.agent = RecyclingClass::Y;
                    action.ticket = loser->order->ticket;
                    action.closeLots = loser->order->lots;
                    action.estimatedPnl = loser->netPnl;
                    action.companionTicket = winner->order->ticket;
                    action.companionLots = closeLots;
                    action.companionEstimatedPnl =
                        winner->netPnl * closeLots / winner->order->lots;
                    action.reason = "extinction par paire";
                    return action;
                }
            }

            if (canSacrifice(*loser) && (loss <= baseBudget || u >= 0.75))
                return makeFullClose(*loser, RecyclingClass::Y,
                                     "sacrifice faute de paire");
        }
    }

    // AGENT Z — split a heavy losing leg first so the remainder can move
    // into lower classes. Full sacrifice is the last, slower fallback.
    if (const auto* worst = worstOverCap(RecyclingClass::Z)) {
        const uint64_t currentTick = tickCount_.load(std::memory_order_relaxed);
        const bool splitRetryDelayed =
            failedZSplitTicket_ == worst->order->ticket &&
            currentTick < zSplitRetryAfterTick_;
        if (!splitRetryDelayed) {
            if (failedZSplitTicket_ == worst->order->ticket)
                failedZSplitTicket_ = -1;
            const double fraction = std::clamp(cfg_.agentZSplitFrac, 0.01, 0.99);
            const double closeLots = normalizeLots(worst->order->lots * fraction);
            const double remainder = worst->order->lots - closeLots;
            const double minLot = std::max(0.0, cfg_.volumeMin);
            if (closeLots >= minLot && remainder + 1e-9 >= minLot &&
                closeLots < worst->order->lots) {
                RecyclingAction action;
                action.type = RecyclingActionType::ClosePartial;
                action.agent = RecyclingClass::Z;
                action.ticket = worst->order->ticket;
                action.closeLots = closeLots;
                action.estimatedPnl = worst->netPnl * closeLots / worst->order->lots;
                action.reason = "fractionnement Z vers Y";
                return action;
            }
        }

        if (cadenceDue(cfg_.agentZCadenceTicks, lastZCycle_, u) &&
            ((flowSign != 0 && worst->direction == -flowSign) || deadlock)) {
            const double loss = -worst->netPnl;
            const double zBudget = baseBudget * 2.0;
            if (loss <= zBudget || u >= 0.75)
                return makeFullClose(*worst, RecyclingClass::Z,
                                     "sacrifice Z anti-derive");
        }
    }

    return std::nullopt;
}

void RecyclingAgents::recordSuccess(const RecyclingAction& action,
                                    double realizedPnl,
                                    const std::string& result) {
    std::lock_guard lock(mutex_);
    if (action.type == RecyclingActionType::ClosePartial &&
        failedZSplitTicket_ == action.ticket)
        failedZSplitTicket_ = -1;
    auto& report = reports_[static_cast<size_t>(action.agent)];
    ++report.actions;
    if (realizedPnl < 0.0) report.sacrificed += -realizedPnl;
    else                  report.harvested += realizedPnl;
    lastResult_ = std::string(className(action.agent)) + ": " + result +
                  " " + std::to_string(realizedPnl) + "$";
}

void RecyclingAgents::recordFailure(const RecyclingAction& action) {
    std::lock_guard lock(mutex_);
    if (action.type == RecyclingActionType::ClosePartial &&
        action.agent == RecyclingClass::Z) {
        failedZSplitTicket_ = action.ticket;
        zSplitRetryAfterTick_ = tickCount_.load(std::memory_order_relaxed) +
            static_cast<uint64_t>(std::max(1, cfg_.agentZCadenceTicks));
    }
    lastResult_ = std::string(className(action.agent)) + ": échec " + action.reason;
}

std::string RecyclingAgents::report(double usdToAccountScale) const {
    std::lock_guard lock(mutex_);
    if (!std::isfinite(usdToAccountScale) || usdToAccountScale <= 0.0)
        usdToAccountScale = 1.0;
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << "Recyclage";
    for (size_t i = 0; i < reports_.size(); ++i) {
        const auto& report = reports_[i];
        out << " " << className(static_cast<RecyclingClass>(i))
            << ":" << report.legs << "/" << report.lots
            << " (" << report.pnl / usdToAccountScale
            << "$; sacrifié=" << report.sacrificed
            << "; récolté=" << report.harvested << ")";
    }
    if (!lastResult_.empty()) out << " | dernier=" << lastResult_;
    return out.str();
}

} // namespace top4ea
