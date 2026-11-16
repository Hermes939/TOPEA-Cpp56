#pragma once

#include "types.hpp"
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace top4ea {

enum class RecyclingClass : uint8_t { X = 0, Y = 1, Z = 2 };
enum class RecyclingActionType : uint8_t { CloseFull, ClosePartial, PairClose };

struct RecyclingAction {
    RecyclingActionType type = RecyclingActionType::CloseFull;
    RecyclingClass agent = RecyclingClass::X;
    int ticket = -1;                 // losing/target leg
    double closeLots = 0.0;
    double estimatedPnl = 0.0;
    int companionTicket = -1;        // winning leg for a pair close
    double companionLots = 0.0;
    double companionEstimatedPnl = 0.0;
    std::string reason;
};

struct RecyclingAgentReport {
    int legs = 0;
    double lots = 0.0;
    double pnl = 0.0;
    double worstPnl = 0.0;
    int worstTicket = -1;
    double sacrificed = 0.0;
    double harvested = 0.0;
    int actions = 0;
};

// Port of the X/Y/Z position recyclers from TOP4EA_AgentsRecycleurs.mqh.
// It plans one action at a time; the engine executes and accounts for it.
class RecyclingAgents {
public:
    explicit RecyclingAgents(Config cfg) : cfg_(std::move(cfg)) {}

    void onTick() noexcept { tickCount_.fetch_add(1, std::memory_order_relaxed); }

    [[nodiscard]] std::optional<RecyclingAction> step(
        const std::vector<OrderInfo>& orders,
        double balance,
        double urgency,
        int flowSign,
        bool monoMode,
        double usdToAccountScale = 1.0);

    void recordSuccess(const RecyclingAction& action,
                       double realizedPnl,
                       const std::string& result);
    void recordFailure(const RecyclingAction& action);

    [[nodiscard]] std::string report(double usdToAccountScale = 1.0) const;

private:
    struct ClassifiedLeg {
        const OrderInfo* order = nullptr;
        RecyclingClass agent = RecyclingClass::X;
        double netPnl = 0.0;
        int direction = 0;
        double cap = 0.0;
    };

    [[nodiscard]] RecyclingClass classify(double lots, double netPnl,
                                          double usdToAccountScale) const noexcept;
    [[nodiscard]] double capFor(RecyclingClass agent, int direction,
                                double urgency, int flowSign,
                                double usdToAccountScale) const noexcept;
    [[nodiscard]] double normalizeLots(double lots) const noexcept;
    [[nodiscard]] bool cadenceDue(int cadence, uint64_t& lastCycle,
                                  double urgency) noexcept;
    void refreshReports(const std::vector<ClassifiedLeg>& legs);
    [[nodiscard]] static const char* className(RecyclingClass agent) noexcept;

    Config cfg_;
    std::atomic<uint64_t> tickCount_{0};
    uint64_t lastXCycle_ = 0;
    uint64_t lastYCycle_ = 0;
    uint64_t lastZCycle_ = 0;
    int failedZSplitTicket_ = -1;
    uint64_t zSplitRetryAfterTick_ = 0;
    TimePoint lastStepAt_{};
    bool haveStepped_ = false;
    mutable std::mutex mutex_;
    std::array<RecyclingAgentReport, 3> reports_{};
    std::string lastResult_;
};

} // namespace top4ea
