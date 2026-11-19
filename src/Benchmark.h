#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <vector>

#ifdef APB_STRESS_TELEMETRY
#include "BudgetPolicy.h"
#endif

class Benchmark
{
public:
    using Clock = std::chrono::steady_clock;

    void Start(Clock::time_point now);
    void Interrupt();
    [[nodiscard]] bool Enabled(Clock::time_point now);
    void Sample(Clock::time_point now, double frameMS, float baseMS, float appliedMS);
#ifdef APB_STRESS_TELEMETRY
    void ObservePolicy(BudgetPolicy::State state);
#endif

private:
    static constexpr std::array<bool, 4> modes{ false, true, true, false };
    static constexpr auto warmup = std::chrono::seconds(6);
    static constexpr auto phaseLength = std::chrono::seconds(30);

    Clock::time_point start_{};
    Clock::time_point nextProbe_{};
    std::vector<double> frameTimes_;
    unsigned phase_{};
    bool finished_{};
};
