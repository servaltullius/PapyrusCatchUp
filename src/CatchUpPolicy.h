#pragma once

#include <algorithm>
#include <cmath>

// Decides how many extra gameplay VM cycles a frame may run so the VM keeps a
// target cycle rate when the game runs below the engage frame rate.
//
// The stock engine runs one VM cycle per frame, and a script that calls a
// native function which cannot run on a tasklet waits for the next cycle.
// While engaged, each frame adds (interval / target - 1) cycles of credit.
//
// Engaging follows a smoothed frame interval, not single frames: a game that
// averages above the engage rate gets no extra cycles from an occasional slow
// frame, and one that averages below it is not dropped by an occasional fast one.
class CatchUpPolicy
{
public:
    static constexpr double minTargetHz = 30.0;
    static constexpr double maxTargetHz = 144.0;
    static constexpr double defaultTargetHz = 60.0;
    static constexpr double defaultEngageHz = 50.0;
    static constexpr unsigned maxPassLimit = 8;
    // Longer gaps are loads, pauses or debugger stops rather than gameplay frames.
    static constexpr double maxIntervalMS = 1000.0;
    // Time constant of the frame-interval average that decides engaging.
    static constexpr double smoothingMS = 500.0;

    CatchUpPolicy() = default;

    CatchUpPolicy(double targetHz, unsigned maxPasses, double engageHz = defaultEngageHz)
    {
        Configure(targetHz, maxPasses, engageHz);
    }

    // The engage rate is clamped to [minTargetHz, targetHz]: catching up toward a rate that the
    // game already exceeds would add nothing.
    void Configure(double targetHz, unsigned maxPasses, double engageHz = defaultEngageHz)
    {
        if (!std::isfinite(targetHz)) {
            targetHz = defaultTargetHz;
        }
        targetHz = std::clamp(targetHz, minTargetHz, maxTargetHz);
        if (!std::isfinite(engageHz)) {
            engageHz = defaultEngageHz;
        }
        engageHz = std::clamp(engageHz, minTargetHz, targetHz);
        targetMS_ = 1000.0 / targetHz;
        engageMS_ = 1000.0 / engageHz;
        maxPasses_ = std::clamp(maxPasses, 1u, maxPassLimit);
        Reset();
    }

    // Called once per stock gameplay VM cycle with the time since the previous one; an interval of
    // zero means there is none yet (the first frame after the gate opened). Returns how many extra
    // cycles this frame may run.
    //
    // `collected` says whether the hooks observed this frame's stock update, which the caller turns
    // on for the next frame while Engaged() holds after this call. A frame they did not observe runs
    // nothing and keeps no credit, so the first observed frame starts with its own share. After the
    // gate opens this gives two frames without extra cycles: one without an interval, and one that
    // only decides to observe.
    [[nodiscard]] unsigned BeginFrame(double intervalMS, bool collected = true)
    {
        if (!std::isfinite(intervalMS) || intervalMS <= 0.0 || intervalMS > maxIntervalMS) {
            Reset();
            return 0;
        }
        if (smoothedMS_ <= 0.0) {
            smoothedMS_ = intervalMS;
        } else {
            const double weight = 1.0 - std::exp(-intervalMS / smoothingMS);
            smoothedMS_ += (intervalMS - smoothedMS_) * weight;
        }
        if (!Engaged() || !collected) {
            credit_ = 0.0;
            return 0;
        }
        credit_ = std::min(credit_ + std::max(0.0, intervalMS / targetMS_ - 1.0), static_cast<double>(maxPasses_));
        return static_cast<unsigned>(credit_);
    }

    void Consume()
    {
        credit_ = std::max(0.0, credit_ - 1.0);
    }

    // The VM had nothing left to resume. Whole cycles owed by idle frames have
    // no work to restore, so only the fractional progress toward the next one is kept.
    void Idle()
    {
        credit_ -= std::floor(credit_);
    }

    // Drops owed cycles but keeps the frame-rate average (holds, pauses after an overrun).
    void DropCredit()
    {
        credit_ = 0.0;
    }

    // Starts over, including the frame-rate average (gate closed, load, invalid interval).
    void Reset()
    {
        credit_ = 0.0;
        smoothedMS_ = 0.0;
    }

    [[nodiscard]] bool Engaged() const { return smoothedMS_ > engageMS_; }
    [[nodiscard]] double Credit() const { return credit_; }
    [[nodiscard]] double TargetMS() const { return targetMS_; }
    [[nodiscard]] double EngageMS() const { return engageMS_; }
    [[nodiscard]] double SmoothedMS() const { return smoothedMS_; }
    [[nodiscard]] unsigned MaxPasses() const { return maxPasses_; }

private:
    double targetMS_{ 1000.0 / defaultTargetHz };
    double engageMS_{ 1000.0 / defaultEngageHz };
    unsigned maxPasses_{ 4 };
    double credit_{};
    double smoothedMS_{};
};
