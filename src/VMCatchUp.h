#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Runs extra stock gameplay VM cycles inside the engine's "VM update" job when
// frames are slower than the target cycle rate.
namespace VMCatchUp
{
    // Which queued script calls an extra cycle may run; see VMCatchUp.cpp.
    enum class Mode : std::uint8_t
    {
        kCarried,     // calls the stock drain of this frame left waiting
        kAccelerate,  // experimental: also a script's next call in the same frame while it only reads
    };

    struct Settings
    {
        Mode mode{ Mode::kCarried };
        // Extra cycles start only while the smoothed frame rate is below this (clamped to targetHz).
        double engageHz{ 50.0 };
        // While engaged, the VM cycle rate extra cycles restore.
        double targetHz{ 60.0 };
        unsigned maxPasses{ 4 };
        double maxExtraMS{ 2.0 };
        unsigned logIntervalSeconds{ 60 };
        // Test builds only (accelerate): also patches the call with which a tasklet queues a native
        // call (AttemptFunctionCall +0x633), so that SetHoldChangeCalls can judge a queued call
        // before it runs. The release plugin leaves this false and installs no such hook.
        bool observeQueuedCalls{ false };
    };

    struct Stats
    {
        std::uint64_t frames;
        std::uint64_t slowFrames;
        // Frames whose smoothed frame rate was below the engage rate.
        std::uint64_t engagedFrames;
        std::uint64_t cycles;
        std::uint64_t taskletCycles;
        // Queued messages (calls and returns) that extra cycles dispatched.
        std::uint64_t extraCalls;
        // Tasklets that were already waiting when an extra cycle started, summed over the cycles.
        // The cycle's own calls did not resume them: they hit the engine's op limit in the previous
        // batch or a latent call returned to them. The cycle's tasklet batch runs them all the same.
        std::uint64_t leftoverTasklets;
        // Frames or cycles that ended with nothing queued.
        std::uint64_t idleStops;
        std::uint64_t timeStops;
        // Extra cycles that ended because a call that must wait reached the front of the queue.
        std::uint64_t blockedStops;
        // Frames that started no extra cycle because the front call already had to wait.
        std::uint64_t blockedSkips;
        // Calls an extra cycle ran against the mode's rule; the first one turns extra cycles off.
        std::uint64_t guardViolations;
        // accelerate: state-changing natives a tasklet called directly, each marking its script.
        std::uint64_t directChanges;
        // Test control SetHoldChangeCalls: checks that held a call only because the call itself is a
        // change (its script was neither new nor marked this frame).
        std::uint64_t changeCallHolds;
        // Skip rule (SetSkipBlocked): waiting calls an extra cycle moved behind the others so
        // that calls behind them could run. Putting the queue in order afterwards is not counted.
        std::uint64_t frontSkips;
        // Skip rule: extra cycles after which the linked queue was rebuilt, because the calls the
        // cycle moved behind were not its front in the order they were moved.
        std::uint64_t orderRebuilds;
        std::uint64_t frozenStops;
        std::uint64_t heldFrames;
        std::uint64_t overrunPauses;
        // Times the host's gate was found closed after a frame in which it was open. Each one
        // restarts the frame-rate average and drops the owed cycles, so the two frames after the
        // gate opens again run no extra cycle.
        std::uint64_t gateCloses;
        double extraMS;
        double maxFrameExtraMS;
        double intervalMS;
        double maxUpdateMS;
        double maxBatchMS;
        std::uint32_t maxBatchTasklets;
        std::uint32_t slowestBatchTasklets;
    };

    // Returns true when the host considers the game in normal gameplay.
    using GateFn = bool();

    [[nodiscard]] Settings Sanitize(Settings settings);

    // Verifies the AE 1.6.1170 job code before patching it. Call from kPostPostLoad.
    bool Install(const Settings& settings, GateFn* gate);

    [[nodiscard]] bool Installed();
    // Call when a save finishes loading, a new game starts or a loading screen closes. Extra cycles
    // wait out the script burst that follows.
    void NotifyGameLoaded();
    Stats ReadAndResetStats();
    // Changes the target cycle rate at runtime (range-checked like the INI value); resets the credit.
    void SetTargetHz(double targetHz);
    // Test control: with false, every check still runs but no extra cycle is executed.
    void SetCyclesAllowed(bool allowed);
    // Test control (accelerate): with false, natives that tasklets call directly are not judged,
    // so only queued natives can hold a script for the next frame. Default true.
    void SetDirectJudgement(bool enabled);
    // Which natives accelerate counts as reads.
    enum class ReadRule : std::uint8_t
    {
        kReviewed,  // release: the helpers and the reviewed natives of NativeClassification.h, by implementation
        kNames,     // test builds: the earlier rule, by function name and class
    };
    // Test control (accelerate). Default kReviewed; the release plugin never calls this.
    void SetReadRule(ReadRule rule);
    // Test control (accelerate, reviewed rule): with false, reading an auto property counts as a
    // change, as it did before its getter was reviewed. Default true; the release plugin never calls
    // this.
    void SetPropertyReads(bool enabled);
    // Test control (accelerate, name rule only): with false, the classes and prefixes taken over from
    // Papyrus Tweaks are judged by the name alone, as before that list was added. Default true; the
    // release plugin never calls this.
    void SetExclusionList(bool enabled);
    // Test control (accelerate, needs Settings::observeQueuedCalls): with true, a queued native that
    // is judged a change is itself kept out of the extra cycles, not only the call that follows it.
    // Default false.
    void SetHoldChangeCalls(bool enabled);
    // The skip rule (accelerate): with true, an extra cycle does not end at a call that must wait.
    // It moves that call behind the others and runs the calls of the linked queue that may run,
    // wherever they stand in it. After the cycle the calls that had to wait are at the front again,
    // in the order they had, ahead of the calls the cycle did not reach and of calls that were
    // queued while it ran. This gives up the rule that the front of the queue is never skipped: a
    // call can now run before a call of another script that was queued earlier. Default false and
    // without effect outside accelerate. The release plugin calls this once, before Install, when
    // [VMCatchUp] SkipWaitingCalls is set; test builds call it to switch the rule between trials.
    void SetSkipBlocked(bool enabled);

    // Test control: counts the functions that report GetIsNative() and run while it is on, by class
    // and name.
    struct CensusEntry
    {
        std::string className;
        std::string name;
        // Not a native function but the getter or setter of an auto property (or an unbound native's
        // stub): VM-internal function objects that also report GetIsNative().
        bool accessor;
        // What the release rule made of the function object, counted per call: a reviewed read
        // (name, origin and callback all matched) or a change.
        std::uint64_t readVerdicts;
        std::uint64_t changeVerdicts;
        std::uint64_t queued;   // run from the queue by a stock update
        std::uint64_t extra;    // run from the queue by an extra cycle
        std::uint64_t direct;   // called directly by a tasklet
        std::uint64_t other;    // any other path, or a frame the hooks did not collect in
    };
    void SetCensus(bool enabled);
    // Returns the counts since the last call and clears them.
    std::vector<CensusEntry> TakeCensus();
    // Census (accelerate): why the call at the front of the queue had to wait, one count per front
    // check that ended so; with the skip rule, one count per call an extra cycle moved behind. The
    // reason is "class.name" of the native that marked the script as changed this frame, or a text
    // in parentheses (script started this frame, ...). Clears the counts.
    std::vector<std::pair<std::string, std::uint64_t>> TakeBlockers();
}
