#pragma once

#include <chrono>

namespace RealBenchmark
{
    using Clock = std::chrono::steady_clock;
    bool Update(Clock::time_point now, Clock::duration interval, float engineBudget);
    void Interrupt();
}
