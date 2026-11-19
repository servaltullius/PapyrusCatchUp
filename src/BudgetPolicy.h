#pragma once

#include <algorithm>
#include <cmath>

class BudgetPolicy
{
public:
    static constexpr double thresholdMS = 20.0;  // 50 engine frames per second
    static constexpr double maxMultiplier = 1.4;

    double Update(double frameMS, bool enabled)
    {
        if (!enabled || !std::isfinite(frameMS) || frameMS <= 0.0 || frameMS > 100.0) {
            Reset();
            return 1.0;
        }

        const double weight = 1.0 - std::exp(-frameMS / 250.0);
        averageMS_ += (frameMS - averageMS_) * weight;
        cooldownMS_ = std::max(0.0, cooldownMS_ - frameMS);
        if (averageMS_ <= thresholdMS || cooldownMS_ > 0.0) {
            slowMS_ = 0.0;
            return 1.0;
        }

        slowMS_ += frameMS;
        if (slowMS_ < 600.0) {
            referenceMS_ = averageMS_;
            return 1.0;
        }

        // Yield to sustained frame-time regressions after granting more VM time.
        if (averageMS_ > referenceMS_ * 1.12) {
            cooldownMS_ = 3000.0;
            slowMS_ = 0.0;
            return 1.0;
        }

        return std::clamp(std::min(frameMS, averageMS_) / thresholdMS, 1.0, maxMultiplier);
    }

    void Reset()
    {
        *this = {};
    }

#ifdef APB_STRESS_TELEMETRY
    enum class State { Disabled, Invalid, Fast, Dwell, Cooldown, Eligible };

    [[nodiscard]] State Observe(double frameMS, bool enabled) const
    {
        if (!enabled) return State::Disabled;
        if (!std::isfinite(frameMS) || frameMS <= 0.0 || frameMS > 100.0) return State::Invalid;
        if (cooldownMS_ > 0.0) return State::Cooldown;
        if (averageMS_ <= thresholdMS || frameMS <= thresholdMS) return State::Fast;
        if (slowMS_ < 600.0) return State::Dwell;
        return State::Eligible;
    }
#endif

private:
    double averageMS_{ thresholdMS };
    double slowMS_{};
    double referenceMS_{ thresholdMS };
    double cooldownMS_{};
};
