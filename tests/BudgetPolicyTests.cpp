#include "BudgetPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace
{
    void Check(bool condition, std::string_view description)
    {
        if (!condition) {
            std::cerr << description << '\n';
            std::exit(1);
        }
    }

    double Repeat(BudgetPolicy& policy, double frameMS, int frames, bool enabled = true)
    {
        double multiplier = 1.0;
        for (int i = 0; i < frames; ++i) {
            multiplier = policy.Update(frameMS, enabled);
        }
        return multiplier;
    }
}

int main()
{
    BudgetPolicy policy;
    Check(Repeat(policy, 1000.0 / 60.0, 100) == 1.0, "60 FPS must pass through");
    Check(Repeat(policy, 20.0, 100) == 1.0, "Exactly 50 FPS must pass through");
    Check(Repeat(policy, 25.0, 10) == 1.0, "Transient dip must pass through");
    Check(std::abs(Repeat(policy, 25.0, 100) - 1.25) < 0.001, "40 FPS needs 1.25x");
    Check(policy.Update(20.0, true) == 1.0, "Do not boost on a frame at the threshold");
    Check(policy.Update(25.0, false) == 1.0, "Disabled mode must pass through");
    Check(policy.Update(25.0, true) == 1.0, "Disabled mode must reset the dwell time");

    policy.Reset();
    Check(Repeat(policy, 1000.0 / 30.0, 100) == 1.4, "30 FPS must be capped at 1.4x");
    Check(policy.Update(160.0, true) == 1.0, "Long stalls must be excluded");
    Check(policy.Update(std::numeric_limits<double>::quiet_NaN(), true) == 1.0, "Invalid timing must pass through");

    Check(Repeat(policy, 1000.0 / 42.0, 100) > 1.0, "Stable 42 FPS must get a boost");
    Check(Repeat(policy, 32.0, 30) == 1.0, "Sustained regression must trigger cooldown");
    Check(Repeat(policy, 1000.0 / 42.0, 180) > 1.0, "Stable timing must recover after cooldown");
    std::cout << "Budget policy checks passed\n";
}
