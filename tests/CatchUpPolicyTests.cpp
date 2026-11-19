#include "CatchUpPolicy.h"
#include "IniValues.h"
#include "LogHistory.h"
#include "MenuGate.h"
#include "NativeClassification.h"
#include "QueueOrder.h"
#include "SlowFrameReport.h"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    void Check(bool condition, std::string_view description)
    {
        if (!condition) {
            std::cerr << description << '\n';
            std::exit(1);
        }
    }

    // Simulates frames where every allowed cycle finds work, and returns the cycles run.
    unsigned RunBusy(CatchUpPolicy& policy, double frameMS, int frames)
    {
        unsigned cycles = 0;
        for (int i = 0; i < frames; ++i) {
            const unsigned allowed = policy.BeginFrame(frameMS);
            for (unsigned pass = 0; pass < allowed; ++pass) {
                policy.Consume();
                ++cycles;
            }
        }
        return cycles;
    }

    bool Near(double value, double expected, double tolerance)
    {
        return std::abs(value - expected) <= tolerance;
    }

    // One extra cycle of the skip rule (VMCatchUp::SetSkipBlocked) on a model of the linked queue.
    // A negative key is a call that must wait, a positive key one that may run. The drain runs at
    // most `budget` calls; `arrivals` are calls queued while the cycle runs, each after the given
    // number of calls ran. Returns the queue after the cycle and its restoration.
    std::vector<int> SkipCycle(std::vector<int> queue, unsigned budget, const std::vector<std::pair<unsigned, int>>& arrivals = {})
    {
        std::vector<int> moved;
        unsigned ran = 0;
        while (ran < budget && !queue.empty()) {
            const int front = queue.front();
            if (std::ranges::find(moved, front) != moved.end()) {
                break;  // once around the queue
            }
            queue.erase(queue.begin());
            if (front < 0) {
                queue.push_back(front);
                moved.push_back(front);
                continue;
            }
            ++ran;
            for (const auto& [after, key] : arrivals) {
                if (after == ran) {
                    queue.push_back(key);
                }
            }
        }
        std::vector<int> restored;
        for (const auto index : QueueOrder::Restored<int>(queue, moved)) {
            restored.push_back(queue[index]);
        }
        return restored;
    }

    // The frame sequence of the VM job hook: the hooks collect during a frame only when the previous
    // frame ended engaged, and a closed gate resets everything. Every allowed cycle finds work.
    struct HookFrames
    {
        CatchUpPolicy policy{ 60.0, 4, 50.0 };
        bool collecting{};

        // An interval of zero is the first frame after the gate opened. Returns the cycles run.
        unsigned Frame(double intervalMS)
        {
            const unsigned allowed = policy.BeginFrame(intervalMS, collecting);
            for (unsigned pass = 0; pass < allowed; ++pass) {
                policy.Consume();
            }
            collecting = policy.Engaged();
            return allowed;
        }

        void GateClosed()
        {
            policy.Reset();
            collecting = false;
        }
    };
}

int main()
{
    CatchUpPolicy policy(50.0, 4);
    Check(RunBusy(policy, 1000.0 / 60.0, 600) == 0, "60 FPS must not add cycles");
    Check(RunBusy(policy, 20.0, 600) == 0, "Exactly the target rate must not add cycles");

    // 42 FPS for 10 seconds: stock gives 420 cycles, the target needs 500.
    policy.Reset();
    const unsigned at42 = RunBusy(policy, 1000.0 / 42.0, 420);
    Check(at42 >= 79 && at42 <= 80, "42 FPS must add the missing 80 cycles over 10 seconds");

    // 25 FPS: one extra cycle every frame.
    policy.Reset();
    Check(RunBusy(policy, 40.0, 250) == 250, "25 FPS must double the cycle rate");

    // 10 FPS: four extra cycles per frame reach 50 Hz exactly.
    policy.Reset();
    Check(RunBusy(policy, 100.0, 10) == 40, "10 FPS must reach the target with four extra cycles");

    // Below the reachable rate the per-frame limit holds.
    policy.Reset();
    Check(policy.BeginFrame(400.0) == 4, "Very slow frames must be capped at the pass limit");
    Check(policy.BeginFrame(400.0) == 4, "Unused credit must not exceed the pass limit");

    // Idle frames must not build a burst.
    policy.Reset();
    for (int i = 0; i < 100; ++i) {
        if (policy.BeginFrame(40.0) > 0) {
            policy.Idle();
        }
    }
    Check(policy.Credit() < 1.0, "Idle frames must not keep whole cycles of credit");
    Check(policy.BeginFrame(40.0) == 1, "Work after idle frames must get only the current frame's cycle");

    // Invalid samples, pauses and loads reset the credit.
    policy.Reset();
    (void)policy.BeginFrame(100.0);
    Check(policy.BeginFrame(std::numeric_limits<double>::quiet_NaN()) == 0, "NaN must not add cycles");
    Check(policy.Credit() == 0.0, "NaN must reset credit");
    (void)policy.BeginFrame(100.0);
    Check(policy.BeginFrame(-1.0) == 0 && policy.Credit() == 0.0, "Negative intervals must reset credit");
    (void)policy.BeginFrame(100.0);
    Check(policy.BeginFrame(1500.0) == 0 && policy.Credit() == 0.0, "Pauses longer than a second must reset credit");

    // Consume never drives credit below zero.
    policy.Reset();
    policy.Consume();
    Check(policy.Credit() == 0.0, "Consume must not go negative");

    // Configuration is clamped to the supported range.
    CatchUpPolicy low(5.0, 0);
    Check(Near(low.TargetMS(), 1000.0 / CatchUpPolicy::minTargetHz, 1e-9) && low.MaxPasses() == 1,
        "Too-low settings must clamp to the minimum target and one pass");
    CatchUpPolicy high(1000.0, 99);
    Check(Near(high.TargetMS(), 1000.0 / CatchUpPolicy::maxTargetHz, 1e-9) && high.MaxPasses() == CatchUpPolicy::maxPassLimit,
        "Too-high settings must clamp to the maximum target and pass limit");
    CatchUpPolicy invalid(std::numeric_limits<double>::infinity(), 4);
    Check(Near(invalid.TargetMS(), 1000.0 / 60.0, 1e-9), "A non-finite target must fall back to 60 Hz");
    Check(Near(CatchUpPolicy{}.TargetMS(), 1000.0 / 60.0, 1e-9), "The default target must be 60 Hz");

    // Engaging: target 60 Hz, engage below 50 FPS.
    CatchUpPolicy split(60.0, 4, 50.0);
    Check(RunBusy(split, 1000.0 / 55.0, 550) == 0 && !split.Engaged(), "55 FPS is above the engage rate: no extra cycles");
    Check(RunBusy(split, 20.0, 500) == 0, "Exactly 50 FPS must not engage");
    split.Reset();
    Check(RunBusy(split, 1000.0 / 45.0, 450) >= 149 && split.Engaged(), "45 FPS engages and catches up toward 60 Hz");
    // One slow frame in a 60 FPS game does not engage.
    split.Reset();
    (void)RunBusy(split, 1000.0 / 60.0, 120);
    Check(split.BeginFrame(40.0) == 0 && !split.Engaged(), "A single 40 ms frame at 60 FPS must not engage");
    // A sustained drop engages within about one time constant; a single fast frame does not drop it.
    split.Reset();
    (void)RunBusy(split, 1000.0 / 60.0, 120);
    unsigned framesToEngage = 0;
    while (!split.Engaged() && framesToEngage < 100) {
        (void)split.BeginFrame(1000.0 / 30.0);
        ++framesToEngage;
    }
    Check(split.Engaged() && framesToEngage <= 30, "A drop to 30 FPS engages within a second");
    (void)split.BeginFrame(1000.0 / 60.0);
    Check(split.Engaged(), "One fast frame does not disengage");
    // Dropping credit keeps the average; a reset clears it.
    split.DropCredit();
    Check(split.Credit() == 0.0 && split.Engaged(), "DropCredit keeps the frame-rate average");
    split.Reset();
    Check(!split.Engaged() && split.SmoothedMS() == 0.0, "Reset clears the frame-rate average");
    // The engage rate never exceeds the target rate, and invalid values fall back to 50.
    Check(Near(CatchUpPolicy(50.0, 4, 60.0).EngageMS(), 20.0, 1e-9), "Engage above target clamps to the target");
    Check(Near(CatchUpPolicy(60.0, 4, std::numeric_limits<double>::quiet_NaN()).EngageMS(), 20.0, 1e-9),
        "A non-finite engage rate falls back to 50");
    Check(Near(CatchUpPolicy{}.EngageMS(), 20.0, 1e-9), "The default engage rate is 50");

    // Hook frame sequence at 30 FPS after the gate opens: a frame without an interval, a frame that
    // only decides to collect, then one extra cycle per frame.
    constexpr double slowMS = 1000.0 / 30.0;
    HookFrames hook;
    Check(hook.Frame(0.0) == 0 && !hook.collecting, "The first frame after the gate opens has no interval and does not collect");
    Check(hook.Frame(slowMS) == 0 && hook.collecting, "The first measured slow frame runs nothing and turns collecting on");
    Check(hook.policy.Credit() == 0.0, "A frame that was not collected keeps no credit");
    Check(hook.Frame(slowMS) == 1, "The first collected frame runs its own cycle only, not the one before it");
    Check(hook.Frame(slowMS) == 1 && hook.Frame(slowMS) == 1, "Collected slow frames keep one cycle per frame");
    // A closed gate starts the sequence over.
    hook.GateClosed();
    Check(hook.Frame(0.0) == 0 && hook.Frame(slowMS) == 0 && hook.Frame(slowMS) == 1,
        "After the gate reopens two frames pass before an extra cycle");
    // A pause longer than a second is not a frame: the average and collecting start over.
    Check(hook.Frame(1500.0) == 0 && !hook.collecting, "A long gap stops collecting");
    Check(hook.Frame(slowMS) == 0 && hook.Frame(slowMS) == 1, "After a long gap one frame decides and the next runs");
    // Above the engage rate nothing is collected and nothing runs, however long it lasts.
    HookFrames fast;
    unsigned fastCycles = fast.Frame(0.0);
    bool fastCollected = false;
    for (int i = 0; i < 600; ++i) {
        fastCycles += fast.Frame(1000.0 / 55.0);
        fastCollected = fastCollected || fast.collecting;
    }
    Check(fastCycles == 0 && !fastCollected, "55 FPS never collects and never runs an extra cycle");
    // Recovering above the engage rate turns collecting off again, and no cycle runs while it is off.
    HookFrames recover;
    (void)recover.Frame(0.0);
    for (int i = 0; i < 60; ++i) {
        (void)recover.Frame(slowMS);
    }
    Check(recover.collecting, "A sustained 30 FPS collects");
    int framesToStop = 0;
    while (recover.collecting && framesToStop < 600) {
        (void)recover.Frame(1000.0 / 60.0);
        ++framesToStop;
    }
    Check(!recover.collecting && framesToStop < 600, "Back at 60 FPS collecting stops");
    Check(recover.Frame(1000.0 / 60.0) == 0, "No extra cycle runs once collecting stopped");
    // The policy itself refuses a frame that was not collected, whatever the caller does afterwards.
    CatchUpPolicy refused(60.0, 4, 50.0);
    for (int i = 0; i < 30; ++i) {
        Check(refused.BeginFrame(slowMS, false) == 0, "A frame the hooks did not collect in gets no extra cycle");
    }
    Check(refused.Engaged() && refused.BeginFrame(slowMS, true) == 1, "Uncollected frames build no burst");

    // Changing the target changes the owed cycles: 60 Hz at 30 FPS needs one per frame.
    CatchUpPolicy sixty(60.0, 4);
    Check(RunBusy(sixty, 1000.0 / 30.0, 300) == 300, "60 Hz at 30 FPS must add one cycle per frame");

    // accelerate mode: reads by name continue within a frame; everything else waits for the next frame.
    for (const auto* name : { "GetPlayer", "getplayer", "HasKeyword", "IsEquipped", "FindClosestActor",
             "CountLinkedRefChain", "WornHasKeyword", "GetNodeScale", "GetEquippedWeapon", "IsLocked",
             "IsEnabled", "GetPlayerGrabbedRef" }) {
        Check(IsReadOnlyNativeName(name), "Read natives must be classified as reads");
    }
    for (const auto* name : { "EquipItem", "UnequipItem", "SetNodeScale", "AddItem", "RemoveItem", "MoveTo",
             "PlaceAtMe", "Wait", "SetPosition", "Disable", "Enable", "SendModEvent", "OnObjectEquipped",
             "", "Get", "is", "Has", "IssueOrder", "Hasten", "CounterAttack", "Getaway", "GetOrCreateActor",
             "GetAndSetValue", "FindAndRemoveItem", "IsResettable", "GetAndIncrement", "FindAndToggle",
             "GetDecremented" }) {
        Check(!IsReadOnlyNativeName(name), "State-changing, empty and bare-prefix names must not be reads");
    }
    // Lowercase pool spellings cannot show word boundaries; the prefix alone decides.
    Check(IsReadOnlyNativeName("haskeyword") && IsReadOnlyNativeName("getformid"), "Lowercase read names stay reads");
    Check(IsReadOnlyNativeName("Get3DScale"), "A digit after the verb starts a new word");

    // Natives a tasklet calls directly: helper classes without side effects stay free.
    Check(IsSideEffectFreeNative("Math", "Floor") && IsSideEffectFreeNative("math", "abs") &&
              IsSideEffectFreeNative("StringUtil", "Substring") && IsSideEffectFreeNative("Debug", "Trace") &&
              IsSideEffectFreeNative("Actor", "GetActorValue"),
        "Helpers and reads must not mark a script as changed");
    Check(!IsSideEffectFreeNative("StorageUtil", "SetIntValue") && !IsSideEffectFreeNative("Utility", "SetINIFloat") &&
              !IsSideEffectFreeNative("Debug", "SendAnimationEvent") && !IsSideEffectFreeNative("Utility", "Wait") &&
              !IsSideEffectFreeNative("", "") && !IsSideEffectFreeNative("Math", "") && !IsSideEffectFreeNative("JMap", "setInt"),
        "State changes, unknown and empty names must mark a script as changed");

    // Classes and prefixes Papyrus Tweaks NG keeps out of its speed-up count as changes here too,
    // even when the name reads like a getter.
    Check(!IsSideEffectFreeNative("Quest", "GetStage") && !IsSideEffectFreeNative("quest", "GetStageDone") &&
              !IsSideEffectFreeNative("Quest", "IsRunning") && !IsSideEffectFreeNative("UI", "IsMenuOpen") &&
              !IsSideEffectFreeNative("Input", "IsKeyPressed") && !IsSideEffectFreeNative("Utility", "GetCurrentRealTime") &&
              !IsSideEffectFreeNative("Debug", "GetVersionNumber") && !IsSideEffectFreeNative("PO3_SKSEFunctions", "GetActorState") &&
              !IsSideEffectFreeNative("ConsoleUtil", "GetSelectedReference") && !IsSideEffectFreeNative("MfgConsoleFunc", "GetPhoneme") &&
              !IsSideEffectFreeNative("PapyrusTweaks", "IsNativeCallSpeedUpActive"),
        "Getters of classes Papyrus Tweaks excludes must mark a script as changed");
    Check(!IsSideEffectFreeNative("ObjectReference", "GetAnimationVariableBool") && !IsSideEffectFreeNative("Actor", "getanimationvariableint") &&
              !IsSideEffectFreeNative("Quest", "GetQuest") && !IsSideEffectFreeNative("SomeMod", "GetQuestObject"),
        "Names with a prefix Papyrus Tweaks excludes must mark a script as changed");
    // The exclusion goes by class and by those two prefixes only.
    Check(IsSideEffectFreeNative("ReferenceAlias", "GetOwningQuest") && IsSideEffectFreeNative("Actor", "GetAnimal") &&
              IsSideEffectFreeNative("ObjectReference", "GetQuality") && IsSideEffectFreeNative("Form", "HasKeyword"),
        "Other getters stay reads");
    // Trace and TraceStack are tasklet natives in the stock game and stay free; the other Debug
    // functions and the queued random-number natives of Utility follow their class.
    Check(IsSideEffectFreeNative("Debug", "Trace") && IsSideEffectFreeNative("debug", "tracestack"),
        "The stock tasklet log natives stay free");
    Check(!IsSideEffectFreeNative("Utility", "RandomInt") && !IsSideEffectFreeNative("Utility", "RandomFloat") &&
              !IsSideEffectFreeNative("Debug", "TraceUser") && !IsSideEffectFreeNative("Debug", "Notification"),
        "Queued natives of excluded classes mark a script as changed");
    // Test control: without the Papyrus Tweaks list its classes fall back to the name rule; the
    // known-change list and the change verbs still apply.
    Check(IsSideEffectFreeNative("Quest", "GetStage", false) && IsSideEffectFreeNative("Utility", "IsInMenuMode", false) &&
              IsSideEffectFreeNative("ObjectReference", "GetAnimationVariableBool", false) &&
              !IsSideEffectFreeNative("Quest", "SetStage", false) && !IsSideEffectFreeNative("Utility", "Wait", false) &&
              !IsSideEffectFreeNative("WornObject", "GetDisplayName", false) && !IsSideEffectFreeNative("Utility", "IsInMenuMode"),
        "The exclusion list can be left out for a measurement without touching the other rules");
    // The reviewed reads (release rule): found by class and name whatever the spelling's case.
    Check(IsHelperNative("Math", "Floor") && IsHelperNative("StringUtil", "Find") && IsHelperNative("Debug", "Trace") &&
              IsHelperNative("debug", "tracestack") && !IsHelperNative("Debug", "Notification") && !IsHelperNative("", "Floor"),
        "The helper natives are the arithmetic, string and log natives only");
    {
        const auto* keyword = FindReviewedRead("form", "haskeyword");
        const auto* effect = FindReviewedRead("SPELL", "GetNthEffectMagicEffect");
        const auto* find = FindReviewedRead("FormList", "Find");
        Check(find && find->origin == NativeOrigin::kGame && find->callbackID == 55669, "FormList.Find is a reviewed read");
        // The native that [NativePacing] lets tasklets run must not mark its script as changed.
        const auto* formID = FindReviewedRead("Form", "GetFormID");
        Check(formID && formID->origin == NativeOrigin::kGame && formID->callbackID == 55902, "Form.GetFormID is a reviewed read");
        Check(keyword && keyword->origin == NativeOrigin::kGame && keyword->callbackID == 55870 &&
                  effect && effect->origin == NativeOrigin::kSKSE && effect->callbackID == 0,
            "A reviewed read carries where its implementation is");
    }
    // Read-like names whose implementation was found to write, or was not read, are not listed.
    Check(!FindReviewedRead("Actor", "GetActorValue") && !FindReviewedRead("Actor", "GetBaseActorValue") &&
              !FindReviewedRead("Actor", "GetActorValuePercentage") && !FindReviewedRead("Actor", "HasSpell") &&
              !FindReviewedRead("ObjectReference", "GetItemCount") && !FindReviewedRead("ObjectReference", "IsInDialogueWithPlayer") &&
              !FindReviewedRead("Keyword", "GetKeyword") && !FindReviewedRead("ObjectReference", "GetDisplayName") &&
              !FindReviewedRead("NetImmerse", "HasNode") && !FindReviewedRead("NetImmerse", "GetNodeScale") &&
              !FindReviewedRead("WornObject", "GetDisplayName") &&
              !FindReviewedRead("SomeMod", "GetValue") && !FindReviewedRead("", "GetValue") && !FindReviewedRead("Actor", ""),
        "A native that was not reviewed, or was found to write, is not a reviewed read whatever its name");
    {
        // The table itself: every game entry names its callback, no callback and no name twice, and no
        // entry contradicts the lists that mark natives as changes under the name rule.
        bool sound = true;
        for (std::size_t i = 0; i < reviewedReads.size(); ++i) {
            const auto& entry = reviewedReads[i];
            sound = sound && !entry.className.empty() && !entry.name.empty();
            sound = sound && (entry.origin == NativeOrigin::kGame ? entry.callbackID != 0 : entry.callbackID == 0);
            sound = sound && !IsHeldBackByPapyrusTweaks(entry.className, entry.name) &&
                !IsKnownChangeBehindReadName(entry.className, entry.name);
            sound = sound && FindReviewedRead(entry.className, entry.name) == &entry;
            for (std::size_t j = 0; j < i; ++j) {
                sound = sound && (entry.callbackID == 0 || entry.callbackID != reviewedReads[j].callbackID);
            }
        }
        Check(sound, "The reviewed-read table must be consistent");
        Check(reviewedSKSEVersion == 0x02020080u, "The reviewed SKSE version is 2.2.8 in SKSE's packed form");
    }
    // A native whose class is not known is a change even when its name reads like a getter.
    Check(!IsSideEffectFreeNative("", "GetValue") && !IsSideEffectFreeNative("", "Trace") && !IsSideEffectFreeNative("Actor", ""),
        "A native without a class or a name must mark a script as changed");
    // Getters whose source was read and found to write item data.
    Check(!IsSideEffectFreeNative("WornObject", "GetDisplayName") && !IsSideEffectFreeNative("wornobject", "getdisplayname") &&
              !IsSideEffectFreeNative("NiOverride", "GetItemUniqueID") && !IsSideEffectFreeNative("NiOverride", "GetObjectUniqueID"),
        "Getters known to write item data must mark a script as changed");
    Check(IsSideEffectFreeNative("ObjectReference", "GetDisplayName") && IsSideEffectFreeNative("WornObject", "GetItemHealthPercent"),
        "The known-change list goes by class and name together");

    // Skip rule (test control): after a cycle the calls that had to wait stand in front in the order
    // they had, then the calls the cycle did not reach, then calls queued while it ran.
    using Queue = std::vector<int>;
    Check(SkipCycle({ -1, 2, -3, 4 }, 10) == Queue{ -1, -3 }, "A full cycle leaves the waiting calls in their order");
    Check(SkipCycle({ 1, 2, 3 }, 10).empty() && SkipCycle({ -1, -2 }, 10) == Queue{ -1, -2 },
        "Without waiting calls everything runs; without calls that may run nothing changes");
    Check(SkipCycle({ -1, 2, -3, 4 }, 10, { { 1, -9 } }) == Queue{ -1, -3, -9 },
        "A call queued during the cycle must not get ahead of a call that was already waiting");
    Check(SkipCycle({ -1, 2, -3, 4 }, 10, { { 1, 9 } }) == Queue{ -1, -3, 9 },
        "A call queued during the cycle stands behind the waiting calls even when it may run; the next cycle reaches it");
    Check(SkipCycle({ -1, 2, -3, 4, 5 }, 1) == Queue{ -1, -3, 4, 5 },
        "A cycle that ran out of budget leaves the queue as it was, without the call that ran");
    Check(SkipCycle({ -1, 2, -3, 4, 5 }, 1, { { 1, 7 } }) == Queue{ -1, -3, 4, 5, 7 },
        "After a cycle that ran out of budget a new call stands behind the calls that were not reached");
    {
        const Queue queued{ 5, 7, 5, 9 };
        const Queue moved{ 9, 3, 5 };
        const auto order = QueueOrder::Restored<int>(queued, moved);
        Check(order == std::vector<std::size_t>{ 3, 0, 1, 2 }, "A moved key that is not queued is left out, and a repeated key keeps its place");
        Check(QueueOrder::Restored<int>(Queue{}, moved).empty() && QueueOrder::Restored<int>(queued, Queue{}) == std::vector<std::size_t>{ 0, 1, 2, 3 },
            "An empty queue stays empty and a queue without moved calls stays as it is");
    }

    // INI switches: only clear words count, so a typo can neither enable nor silently pass as a value.
    Check(IniValues::ParseSwitch(L"true") == true && IniValues::ParseSwitch(L"YES") == true &&
              IniValues::ParseSwitch(L"On") == true && IniValues::ParseSwitch(L"1") == true,
        "Clear on-values must read as true");
    Check(IniValues::ParseSwitch(L"false") == false && IniValues::ParseSwitch(L"No") == false &&
              IniValues::ParseSwitch(L"OFF") == false && IniValues::ParseSwitch(L"0") == false,
        "Clear off-values must read as false");
    for (const auto* value : { L"flase", L"ture", L"2", L"", L"enabled", L"true1", L"t" }) {
        Check(!IniValues::ParseSwitch(value), "Anything else is not a switch value");
    }

    // INI numbers must not follow the process locale: a comma-decimal locale still reads "2.0".
    Check(std::setlocale(LC_ALL, "de-DE") != nullptr, "The comma-decimal test locale must be available");
    Check(std::wcstod(L"2.5", nullptr) == 2.0, "The test locale must make the C library stop at a decimal point");
    Check(IniValues::ParseNumber(L"2.0") == 2.0 && IniValues::ParseNumber(L"0.25") == 0.25 &&
              IniValues::ParseNumber(L"50") == 50.0 && IniValues::ParseNumber(L"+60") == 60.0 &&
              IniValues::ParseNumber(L"-1.5") == -1.5,
        "Decimal numbers must parse with a point in any locale");
    for (const auto* value : { L"2,0", L"", L"abc", L"2.0x", L"1 2", L"+", L"+-5", L"--5", L"２" }) {
        Check(!IniValues::ParseNumber(value), "Non-numeric values must be rejected");
    }
    std::setlocale(LC_ALL, "C");

    // The menu part of the gameplay gate: the loading and main menus by their events, with one
    // reading of the UI after each load.
    {
        MenuLatch latch;
        Check(latch.Blocked(), "The gate is closed until the UI has been read once");
        latch.SetMain(true);
        latch.SetLoading(true);
        latch.SetMain(false);
        Check(latch.Blocked(), "Events alone do not open the gate before the first reading");
        latch.Sync(latch.Ask(), true, false);
        Check(latch.Blocked(), "A reading that finds the loading menu open keeps the gate closed");
        latch.SetLoading(false);
        Check(!latch.Blocked(), "The loading menu closing opens the gate without another reading");

        latch.SetLoading(true);
        Check(latch.Blocked(), "The loading menu opening closes the gate at once");
        latch.SetLoading(false);
        latch.SetMain(true);
        Check(latch.Blocked(), "The main menu opening closes the gate at once");
        latch.SetMain(false);
        Check(!latch.Blocked(), "The main menu closing opens it again");

        // A reading that was taken while the loading menu was open, applied after its close event.
        latch.SetLoading(true);
        const auto early = latch.Ask();
        latch.SetLoading(false);
        latch.Sync(early, true, false);
        Check(!latch.Blocked(), "A reading cannot overrule a menu event that arrived after it began");

        // The same for the other menu, while the reading still sets the menu no event spoke of.
        const auto mixed = latch.Ask();
        latch.SetMain(true);
        latch.Sync(mixed, true, false);
        Check(latch.Blocked(), "The reading sets the menu that had no event since it began");
        latch.SetMain(false);
        Check(latch.Blocked(), "and that menu keeps the gate closed after the other one closed");
        latch.SetLoading(false);
        Check(!latch.Blocked(), "until its own close event arrives");

        // A missed close event is put right by the reading after the next load.
        latch.SetLoading(true);
        latch.Sync(latch.Ask(), false, false);
        Check(!latch.Blocked(), "A reading opens a gate that a missed close event left closed");
    }

    // Logs of earlier sessions: the previous log is renamed after its time, only the newest are kept.
    {
        std::tm time{};
        time.tm_year = 126;
        time.tm_mon = 10;
        time.tm_mday = 12;
        time.tm_hour = 14;
        time.tm_min = 5;
        time.tm_sec = 9;
        Check(LogHistory::DatedStem("Plugin", time) == "Plugin-20261112-140509", "A kept log is named after the date and time");
        Check(LogHistory::IsKeptLog("Plugin", "Plugin-20261112-140509.log") && LogHistory::IsKeptLog("Plugin", "Plugin-20261112-140509-2.log"),
            "Kept logs are recognised by their name");
        Check(!LogHistory::IsKeptLog("Plugin", "Plugin.log") && !LogHistory::IsKeptLog("Plugin", "PluginX-20261112-140509.log") &&
                  !LogHistory::IsKeptLog("Plugin", "Plugin-20261112-140509.txt") && !LogHistory::IsKeptLog("Plugin", "Plugin-2026111-140509.log") &&
                  !LogHistory::IsKeptLog("Plugin", "Plugin-20261112-140509-old.log"),
            "The current log and other files are not taken for kept logs");

        namespace fs = std::filesystem;
        const auto folder = fs::temp_directory_path() / "apb-log-history-test";
        std::error_code error;
        fs::remove_all(folder, error);
        fs::create_directories(folder);
        const auto write = [&folder](const char* name, const char* text) { std::ofstream(folder / name) << text; };

        Check(LogHistory::Keep(folder, "Plugin", 3).kept.empty(), "Without a previous log nothing is kept");
        write("Plugin.log", "");
        Check(LogHistory::Keep(folder, "Plugin", 3).kept.empty() && fs::exists(folder / "Plugin.log"), "An empty log is left for the next session");

        write("Plugin.log", "session");
        write("Plugin-20260101-000000.log", "oldest");
        write("Plugin-20260102-000000.log", "older");
        write("Plugin-20260103-000000.log", "old");
        write("Other-20260101-000000.log", "another plugin");
        write("Plugin-notes.log", "not a kept log");
        const auto result = LogHistory::Keep(folder, "Plugin", 3);
        Check(!result.kept.empty() && fs::exists(result.kept) && !fs::exists(folder / "Plugin.log") && result.problem.empty(),
            "The previous log is moved aside");
        Check(LogHistory::IsKeptLog("Plugin", result.kept.filename().string()), "under a dated name");
        Check(result.removed == 1 && !fs::exists(folder / "Plugin-20260101-000000.log") && fs::exists(folder / "Plugin-20260102-000000.log") &&
                  fs::exists(folder / "Plugin-20260103-000000.log"),
            "Only the oldest kept log beyond the limit is deleted");
        Check(fs::exists(folder / "Other-20260101-000000.log") && fs::exists(folder / "Plugin-notes.log"), "Files of other names are not touched");

        // Another plugin's file whose name the narrow code page cannot hold must not stop the clean-up,
        // and a second log of the same second sorts after the first.
        std::ofstream(folder / fs::path(L"\U0001F600 other plugin.log")) << "x";
        write("Plugin.log", "next session");
        const auto second = LogHistory::Keep(folder, "Plugin", 3);
        Check(second.problem.empty() && !second.kept.empty() && second.kept != result.kept, "A folder with such a name is handled without a problem");
        Check(second.removed == 1 && !fs::exists(folder / "Plugin-20260102-000000.log") && fs::exists(folder / "Plugin-20260103-000000.log") &&
                  fs::exists(result.kept) && fs::exists(second.kept),
            "The oldest kept log goes, the two newest stay");
        Check(fs::exists(folder / fs::path(L"\U0001F600 other plugin.log")), "and the other plugin's file is still there");
        Check(LogHistory::IsKeptLog<wchar_t>("Plugin", L"Plugin-20261112-140509-9.log") && !LogHistory::IsKeptLog<wchar_t>("Plugin", L"Plugin-20261112-140509-1.log") &&
                  !LogHistory::IsKeptLog<wchar_t>("Plugin", L"Plugin-20261112-14050\u0E59.log"),
            "Names are checked in the file system's characters");

        // Keeping none means leaving everything alone.
        write("Plugin.log", "kept nowhere");
        const auto none = LogHistory::Keep(folder, "Plugin", 0);
        Check(none.kept.empty() && none.removed == 0 && fs::exists(folder / "Plugin.log") && fs::exists(second.kept), "With zero logs to keep nothing is moved or deleted");

        // A log another program holds open cannot be renamed; it stays, and the result says so.
        write("Plugin.log", "held open");
        {
            std::ifstream held(folder / "Plugin.log");
            const auto locked = LogHistory::Keep(folder, "Plugin", 3);
            Check(locked.kept.empty() && !locked.problem.empty() && fs::exists(folder / "Plugin.log"),
                "A log that cannot be moved is left in place and reported");
        }

        // A subdirectory that looks like a kept log name is skipped without stopping cleanup.
        fs::create_directories(folder / "Plugin-20260101-000001.log", error);
        write("Plugin.log", "with dir");
        const auto withDir = LogHistory::Keep(folder, "Plugin", 3);
        Check(withDir.problem.empty() && !withDir.kept.empty() && withDir.removed == 1 &&
                  fs::is_directory(folder / "Plugin-20260101-000001.log"),
            "Subdirectories with matching names are ignored and do not stop cleanup");

        const auto missing = LogHistory::Keep(folder / "does-not-exist", "Plugin", 3);
        Check(missing.kept.empty() && missing.removed == 0, "A folder that does not exist changes nothing");
        fs::remove_all(folder, error);
    }

    // The names in a slow-frame report.
    Check(SlowFrameReport::Summarize({}, 12) == "none", "A batch without tasklets reports none");
    Check(SlowFrameReport::Summarize({ "B.g", "A.f", "B.g", "", "A.f", "B.g" }, 12) == "B.g x3, A.f x2, (unknown)",
        "Names are counted, the most frequent first, unknown ones named so");
    Check(SlowFrameReport::Summarize({ "", "" }, 12) == "(unknown) x2",
        "Multiple unknown names collapse into one count");
    Check(SlowFrameReport::Summarize({ "A.f", "B.g", "C.h", "D.i" }, 2) == "A.f, B.g (+2 more)",
        "Equal counts keep their order and the rest is counted");

    std::cout << "Catch-up policy checks passed\n";
}
