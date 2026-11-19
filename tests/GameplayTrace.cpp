#include "RE/P/Profiler.h"
#include "RE/T/TESEquipEvent.h"
#include "Continuation.h"
#include "MenuWatch.h"
#include "NativeClassification.h"
#include "NativeDrainTrace.h"
#include "NativePacing.h"
#include "QueueHeadroom.h"
#include "ResumeTrace.h"
#include "VMCatchUp.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <format>
#include <cstdlib>
#include <fstream>

namespace
{
    using namespace std::chrono_literals;

    // Samples every 50 ms whether the game owns the foreground window while a trial runs. A game in
    // the background competes with other windows for the CPU, GPU and presentation, so trials with a
    // low share are suspect.
    class ForegroundSampler
    {
    public:
        ForegroundSampler() :
            thread_([this](std::stop_token stop) {
                const DWORD self = GetCurrentProcessId();
                while (!stop.stop_requested()) {
                    DWORD owner = 0;
                    if (const HWND window = GetForegroundWindow()) {
                        GetWindowThreadProcessId(window, &owner);
                    }
                    samples_.fetch_add(1, std::memory_order_relaxed);
                    if (owner == self) {
                        foreground_.fetch_add(1, std::memory_order_relaxed);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            })
        {}

        // Percentage of samples with the game in front; stops sampling.
        double Stop()
        {
            thread_.request_stop();
            if (thread_.joinable()) {
                thread_.join();
            }
            const auto samples = samples_.load();
            return samples ? 100.0 * static_cast<double>(foreground_.load()) / static_cast<double>(samples) : 0.0;
        }

        ~ForegroundSampler() { (void)Stop(); }

    private:
        std::atomic<std::uint64_t> samples_{};
        std::atomic<std::uint64_t> foreground_{};
        std::jthread thread_;
    };
    using Clock = std::chrono::steady_clock;
    using Profiler = RE::SkyrimScript::Profiler;
    using Query = RE::BSTSmartPointer<RE::BSScript::Internal::IFuncCallQuery>;
    using Value = RE::BSScript::Variable;
    using String = RE::BSFixedString;
    using QueueFn = void(Profiler*, std::uint32_t, std::uint32_t, const Query&);
    using FrameFn = void(Profiler*, std::uint32_t, std::uint32_t, const Value&, const String&, const String&, const String&);
    using UpdateFn = void(void*, void*, void*, void*, float);

    constexpr auto runtime = SKSE::RUNTIME_SSE_1_6_1170;
    constexpr unsigned probeCount = 32;
    constexpr unsigned changes = 12;
    constexpr auto spacing = 250ms;
    // APB_EQUIP_SINGLE=1: instead of twelve changes 250 ms apart, each trial takes the selected item
    // off at its start and puts it back on once, at tick singleEquipTick (4 s later, when the unequip
    // handlers have long finished), so that a trial's drain time is how long the scripts reacting to
    // one equip need: the delay a player sees after a single action.
    bool singleChange{};
    constexpr unsigned singleEquipTick = 16;

    bool IsChangeTick(unsigned i)
    {
        return singleChange ? i == 0 || i == singleEquipTick : i < changes;
    }

    bool IsEquipTick(unsigned i)
    {
        return singleChange ? i == singleEquipTick : (i % 2) != 0;
    }

    unsigned ChangesPerTrial()
    {
        return singleChange ? 2u : changes;
    }
    constexpr std::array sequence{ false, true, true, false, true, false, false, true };
    constexpr std::size_t maxTraceEvents = 131072;

    QueueFn* originalQueue{};
    FrameFn* originalPush{};
    FrameFn* originalPop{};
    std::atomic_bool capture{};
    std::atomic_bool started{};
    std::mutex traceLock;
    std::atomic_uint selectedForm{};
    std::atomic_bool profileActive{};
    std::atomic_bool playerBound{};
    std::atomic_bool promote{};
    REL::Relocation<UpdateFn*> originalUpdate;
    Clock::time_point previousUpdate{};
    bool comparePlayer{};
    bool compareCatchUp{};
    // catchup-target: catch-up stays on; "enabled" trials use target 60 Hz, the others 50 Hz.
    bool compareCatchUpTarget{};
    // catchup-direct: accelerate stays on; "enabled" trials judge natives that tasklets call
    // directly, the others judge queued natives only.
    bool compareCatchUpDirect{};
    // catchup-exclusions: accelerate stays on under the name rule; "enabled" trials use the Papyrus
    // Tweaks exclusion list, the others judge those classes by the name alone.
    bool compareCatchUpExclusions{};
    // catchup-holdcalls: accelerate stays on; "enabled" trials also keep a queued native that is
    // judged a change out of the extra cycles, the others only the call that follows it (release).
    bool compareCatchUpHoldCalls{};
    // catchup-readrule: accelerate stays on; "enabled" trials judge natives by the reviewed reads
    // (release), the others by the earlier name rule.
    bool compareCatchUpReadRule{};
    // catchup-getter: accelerate stays on under the reviewed rule; "enabled" trials count reading an
    // auto property as a read (release), the others as a change.
    bool compareCatchUpGetter{};
    // catchup-skipfront: accelerate stays on; "enabled" trials let an extra cycle pass calls that
    // must wait and run the calls behind them (the skip rule, the release plugin's option
    // SkipWaitingCalls), the others stop at the first such call (the release default).
    bool compareCatchUpSkipFront{};
    // APB_CATCHUP_SKIP=1: the skip rule for every trial of the other modes. Each trial's line
    // says whether the rule was on, so that a value left in the shell cannot pass unnoticed.
    bool skipBlockedWholeRun{};
    // catchup-census: accelerate stays on in every trial and the natives that run are counted by
    // function; the counts are logged when the run ends. Not for timing.
    bool censusCatchUp{};

    // Modes in which catch-up runs in every trial and "enabled" selects a variant of it.
    bool CatchUpAlwaysOn()
    {
        return compareCatchUpTarget || compareCatchUpDirect || compareCatchUpExclusions || compareCatchUpHoldCalls ||
            compareCatchUpReadRule || compareCatchUpGetter || compareCatchUpSkipFront || censusCatchUp;
    }
    // catchup-soak: one launch with catch-up on or off (APB_CATCHUP_ON) for repeated cell travel and
    // equipment changes. The menu part of the gate is the release plugin's own code (MenuWatch.h).
    bool soakCatchUp{};
    bool soakEnabled{};
    // APB_SOAK_JB1=1 (catchup-soak): before each trip, start two Chatty NPCs follower quests whose
    // aliases run JB1ClearAlias.OnCellDetach (GetOwningQuest().Stop() then Clear()), so every trip
    // leaves a cell with them running; their state is logged before travel, after arrival and at the end.
    bool soakJB1{};
    // APB_CATCHUP_ON=none: the workload driver runs but VM catch-up is not installed at all.
    bool installCatchUpModule{ true };
    std::atomic_bool catchUpEnabled{};
    bool compareHeadroom{};
    bool compareRetry{};
    bool compareEntry{};
    bool profileCells{};
    bool compareContinuation{};
    bool replicateContinuation{};
    bool reverseContinuation{};
    bool observeGetAtReturn{};
    bool observeNativeDrain{};
    bool observeNativeStages{};
    bool observeTaskletFinish{};
    bool observeJobBatch{};
    bool observeReleaseSource{};
    bool compareNativeDrain{};
    bool stagedExterior{};
    bool traceResume{};
    bool installed{};

    std::int64_t QPC()
    {
        LARGE_INTEGER value{};
        QueryPerformanceCounter(&value);
        return value.QuadPart;
    }

    struct TraceEvent
    {
        std::int64_t qpc;
        std::uint32_t stack;
        std::uint32_t depth;
        char kind;
        std::uint32_t thread;
        std::uint64_t detail;
        std::array<char, 64> names[3]{};
    };

    std::vector<TraceEvent> events;
    unsigned droppedEvents{};

    void Record(char kind, std::uint32_t stack, std::uint32_t depth,
        const String* a = nullptr, const String* b = nullptr, const String* c = nullptr, std::uint64_t detail = 0)
    {
        if (!capture.load(std::memory_order_relaxed)) {
            return;
        }
        TraceEvent event{ QPC(), stack, depth, kind, traceResume ? GetCurrentThreadId() : 0, detail };
        const std::array names{ a, b, c };
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i]) {
                const std::string_view name{ names[i]->c_str() };
                std::memcpy(event.names[i].data(), name.data(), std::min(name.size(), event.names[i].size() - 1));
            }
        }
        const std::lock_guard guard(traceLock);
        if (!capture.load(std::memory_order_relaxed)) {
            return;
        }
        if (events.size() < events.capacity()) {
            events.push_back(event);
        } else {
            ++droppedEvents;
        }
    }

    void Queue(Profiler* self, std::uint32_t stack, std::uint32_t depth, const Query& query)
    {
        Record('Q', stack, depth);
        originalQueue(self, stack, depth, query);
    }

    void Push(Profiler* self, std::uint32_t stack, std::uint32_t depth,
        const Value& value, const String& a, const String& b, const String& c)
    {
        Record('P', stack, depth, &a, &b, &c);
        originalPush(self, stack, depth, value, a, b, c);
    }

    void Pop(Profiler* self, std::uint32_t stack, std::uint32_t depth,
        const Value& value, const String& a, const String& b, const String& c)
    {
        Record('O', stack, depth, &a, &b, &c);
        originalPop(self, stack, depth, value, a, b, c);
    }

    bool InstallProfiler()
    {
        const auto address = RE::VTABLE_SkyrimScript__Profiler[0].address();
        const std::array expected{ REL::ID(53666).address(), REL::ID(53667).address(),
            REL::ID(53668).address(), REL::ID(53669).address() };
        originalQueue = reinterpret_cast<QueueFn*>(expected[0]);
        originalPush = reinterpret_cast<FrameFn*>(expected[1]);
        originalPop = reinterpret_cast<FrameFn*>(expected[3]);
        const std::array replacement{ reinterpret_cast<std::uintptr_t>(&Queue),
            reinterpret_cast<std::uintptr_t>(&Push), expected[2], reinterpret_cast<std::uintptr_t>(&Pop) };
        return REL::safe_write(address + sizeof(std::uintptr_t), replacement.data(), sizeof(replacement),
            expected.data(), sizeof(expected));
    }

    void Update(void* vm, void* queue, void* otherQueue, void* lock, float budget)
    {
        const auto now = Clock::now();
        const auto interval = now - std::exchange(previousUpdate, now);
        auto* ui = RE::UI::GetSingleton();
        NativePacing::BeginUpdate(promote.load() && interval > 20ms && interval < 100ms && ui &&
            !ui->GameIsPaused() && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));
        originalUpdate(vm, queue, otherQueue, lock, budget);
    }

    bool InstallPlayerExperiment()
    {
        constexpr std::array<std::uint8_t, 5> expected{ 0xE8, 0xD1, 0xF5, 0x00, 0x00 };
        const auto address = REL::ID(53928).address() + 0xBA;
        if (!REL::verify_code(address, expected.data(), expected.size()) || !NativePacing::Install()) {
            return false;
        }
        originalUpdate = SKSE::GetTrampoline().write_call<5>(address, Update);
        return true;
    }

    bool CatchUpGate()
    {
        return catchUpEnabled.load() && !(soakCatchUp && MenuWatch::Blocked());
    }

    // catchup-census: one line per native that ran during the trials, most frequent first, with how
    // each rule judges it.
    void LogCensus()
    {
        auto entries = VMCatchUp::TakeCensus();
        std::ranges::sort(entries, std::ranges::greater{}, [](const VMCatchUp::CensusEntry& entry) {
            return entry.queued + entry.extra + entry.direct + entry.other;
        });
        SKSE::log::info("census natives={}", entries.size());
        for (const auto& entry : entries) {
            // "runtime_read" / "runtime_change" count the release rule's verdict on the function object
            // for every call; "reviewed" is the table lookup by name alone. A listed native with
            // runtime_change above zero was not the reviewed implementation in those calls (another
            // plugin registered it, or SKSE is another version).
            SKSE::log::info("census class={} name={} kind={} runtime_read={} runtime_change={} queued={} extra={} direct={} other={} name_rule={} without_pt_list={} reviewed={}",
                entry.className.empty() ? "-" : entry.className, entry.name.empty() ? "-" : entry.name,
                entry.accessor ? "property" : "native", entry.readVerdicts, entry.changeVerdicts,
                entry.queued, entry.extra, entry.direct, entry.other,
                IsSideEffectFreeNative(entry.className, entry.name) ? "read" : "change",
                IsSideEffectFreeNative(entry.className, entry.name, false) ? "read" : "change",
                IsHelperNative(entry.className, entry.name) || FindReviewedRead(entry.className, entry.name) ? "read" : "change");
        }
        // Why the front of the queue waited: the native that marked the front script, or a text in
        // parentheses. One count per front check that found a call that had to wait, so the sum is
        // blocked_skips plus blocked_stops over the run. With the skip rule on (APB_CATCHUP_SKIP=1)
        // it is one count per call an extra cycle moved behind instead, so the sum is front_skips.
        auto blockers = VMCatchUp::TakeBlockers();
        std::ranges::sort(blockers, std::ranges::greater{}, [](const auto& blocker) { return blocker.second; });
        SKSE::log::info("blockers reasons={}", blockers.size());
        for (const auto& [reason, checks] : blockers) {
            SKSE::log::info("blocker checks={} reason={}", checks, reason);
        }
    }

    // The target and the mode the catch-up module was installed with, for the per-trial log.
    double installedTargetHz{};
    bool installedAccelerate{};

    bool InstallCatchUp()
    {
        VMCatchUp::Settings settings{};
        if (const char* target = std::getenv("APB_CATCHUP_TARGET")) {
            settings.targetHz = std::strtod(target, nullptr);
        }
        if (const char* engage = std::getenv("APB_CATCHUP_ENGAGE")) {
            settings.engageHz = std::strtod(engage, nullptr);
        }
        if (const char* mode = std::getenv("APB_CATCHUP_MODE")) {
            const std::string_view name{ mode };
            if (name != "accelerate" && name != "carried") {
                SKSE::log::error("Unknown APB_CATCHUP_MODE '{}'", name);
                return false;
            }
            settings.mode = name == "accelerate" ? VMCatchUp::Mode::kAccelerate : VMCatchUp::Mode::kCarried;
        }
        settings.logIntervalSeconds = 0;
        settings.observeQueuedCalls = compareCatchUpHoldCalls;
        settings = VMCatchUp::Sanitize(settings);
        installedTargetHz = settings.targetHz;
        installedAccelerate = settings.mode == VMCatchUp::Mode::kAccelerate;
        return VMCatchUp::Install(settings, CatchUpGate);
    }

    class EquipObserver final : public RE::BSTEventSink<RE::TESEquipEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* event,
            RE::BSTEventSource<RE::TESEquipEvent>*) override
        {
            if (event && event->actor && event->actor.get() == RE::PlayerCharacter::GetSingleton() &&
                event->baseObject == selectedForm.load(std::memory_order_relaxed)) {
                Record(event->equipped ? 'E' : 'U', event->baseObject, 0);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    EquipObserver equipObserver;

    class MenuObserver final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (soakCatchUp) {
                // Mirrors the release plugin: a closing loading screen starts the post-load hold.
                if (event && !event->opening && event->menuName == RE::LoadingMenu::MENU_NAME) {
                    VMCatchUp::NotifyGameLoaded();
                }
                MenuWatch::OnEvent(event);
            } else {
                QueueHeadroom::ObserveGameplay(false);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    MenuObserver menuObserver;

    struct Trial
    {
        std::mutex lock;
        std::vector<double> responses;
        unsigned requested{};
        unsigned actions{};
        unsigned failures{};
        unsigned playerID{};
        RE::FormID sourceCell{};
        RE::FormID destinationCell{};
        std::atomic_bool cellCheckPending{};
        std::atomic_bool cellArrived{};
        std::atomic_bool probePending{};
    };

    class PlayerCallback final : public RE::BSScript::IStackCallbackFunctor
    {
    public:
        explicit PlayerCallback(std::shared_ptr<Trial> trial) : trial_(std::move(trial)) {}

        void operator()(Value result) override
        {
            auto* actor = result.Unpack<RE::Actor*>();
            const std::lock_guard guard(trial_->lock);
            trial_->playerID = actor ? actor->GetFormID() : 0;
        }

        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

    private:
        std::shared_ptr<Trial> trial_;
    };

    class ProbeCallback final : public RE::BSScript::IStackCallbackFunctor
    {
    public:
        ProbeCallback(std::shared_ptr<Trial> trial, Clock::time_point start) :
            trial_(std::move(trial)), start_(start)
        {}

        void operator()(Value) override
        {
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
            const std::lock_guard guard(trial_->lock);
            trial_->responses.push_back(elapsed);
        }

        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

    private:
        std::shared_ptr<Trial> trial_;
        Clock::time_point start_;
    };

    struct ProbeQuest
    {
        std::uint32_t id;
        const char* editorID;
        bool start;
    };
    // Chatty NPCs.esp quests whose reference aliases carry JB1ClearAlias. The first two are started.
    constexpr std::array<ProbeQuest, 4> jb1Quests{ {
        { 0x8C2, "JB1FollowerGreeting", true },
        { 0x8B8, "JB1ChattyFollowers", true },
        { 0xD0D, "JB1FollowerEngine", false },
        { 0x937, "JB1FollowerMeh", false },
    } };

    // Main thread. One line: running/starting/stopping/stopped, stage and each alias's reference.
    void LogJB1State(unsigned cycle, const char* when)
    {
        auto* data = RE::TESDataHandler::GetSingleton();
        std::string line;
        for (const auto& probe : jb1Quests) {
            auto* quest = data ? data->LookupForm<RE::TESQuest>(probe.id, "Chatty NPCs.esp") : nullptr;
            if (!quest) {
                line += std::format(" {}=missing", probe.editorID);
                continue;
            }
            line += std::format(" {}=running:{},starting:{},stopping:{},stopped:{},stage:{}", probe.editorID, quest->IsRunning(),
                quest->IsStarting(), quest->IsStopping(), quest->IsStopped(), quest->GetCurrentStageID());
            for (const auto* alias : quest->aliases) {
                if (!alias) {
                    continue;
                }
                std::uint32_t filled = 0;
                if (const auto it = quest->refAliasMap.find(alias->aliasID); it != quest->refAliasMap.end()) {
                    if (const auto reference = it->second.get()) {
                        filled = reference->GetFormID();
                    }
                }
                line += std::format(",{}:{:08X}", alias->aliasName.c_str(), filled);
            }
        }
        SKSE::log::info("jb1 state cycle={} when={}{}", cycle, when, line);
    }

    void Travel(const std::shared_ptr<Trial>& trial, unsigned index)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
        const std::unique_ptr<RE::Script> command(factory ? factory->Create() : nullptr);
        if (!cell || !command) {
            const std::lock_guard guard(trial->lock);
            ++trial->failures;
            return;
        }
        {
            const std::lock_guard guard(trial->lock);
            trial->sourceCell = cell->GetFormID();
            ++trial->actions;
        }
        Record('b', cell->GetFormID(), 0);
        command->SetCommand(index % 2 == 0 ? "coc WhiterunBanneredMare" : "coc WhiterunOrigin");
        command->CompileAndRun(nullptr);
    }

    void CheckArrival(const std::shared_ptr<Trial>& trial)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto* cell = player ? player->GetParentCell() : nullptr;
        auto* ui = RE::UI::GetSingleton();
        if (cell && ui && !ui->GameIsPaused() && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
            const std::lock_guard guard(trial->lock);
            if (trial->sourceCell && cell->GetFormID() != trial->sourceCell) {
                trial->destinationCell = cell->GetFormID();
                Record('c', trial->destinationCell, 0);
                if (compareContinuation) {
                    // The Save93 exterior arrival was verified at this cell in the control run.
                    Continuation::Arm(trial->destinationCell == 0x0001A27F);
                }
                if (compareNativeDrain) {
                    NativeDrainTrace::Arm(trial->destinationCell == 0x0001A27F);
                }
                trial->cellArrived.store(true);
            }
        }
        trial->cellCheckPending.store(false);
    }

    bool AwaitArrival(const std::shared_ptr<Trial>& trial, unsigned index, std::stop_token stop)
    {
        SKSE::GetTaskInterface()->AddTask([trial, index] { Travel(trial, index); });
        const auto deadline = Clock::now() + 35s;
        while (!trial->cellArrived.load() && !stop.stop_requested() && Clock::now() < deadline) {
            if (!trial->cellCheckPending.exchange(true)) {
                SKSE::GetTaskInterface()->AddTask([trial] { CheckArrival(trial); });
            }
            std::this_thread::sleep_for(100ms);
        }
        return trial->cellArrived.load();
    }

    bool AwaitStableCadence(std::stop_token stop, unsigned buckets, std::chrono::seconds timeout)
    {
        const auto frame = [] {
            return static_cast<volatile const std::uint32_t&>(RE::BSGraphics::State::GetSingleton()->GetFrameCount());
        };
        auto before = frame();
        unsigned stable = 0;
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline && !stop.stop_requested()) {
            std::this_thread::sleep_for(500ms);
            const auto now = frame();
            const auto count = now - std::exchange(before, now);
            stable = count >= 19 && count <= 23 ? stable + 1 : 0;
            if (stable == buckets) {
                return true;
            }
        }
        return false;
    }

    void Tick(const std::shared_ptr<Trial>& trial, bool change, bool equip, bool checkPlayer)
    {
        const auto failure = [&trial] {
            const std::lock_guard guard(trial->lock);
            ++trial->failures;
        };
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        const bool ready = vm && ui && !ui->GameIsPaused() && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) && player;
        if (compareHeadroom) {
            QueueHeadroom::ObserveGameplay(ready);
        }
        if (!ready) {
            if (!profileCells) {
                failure();
            }
            return;
        }
        if (change) {
            auto* manager = RE::ActorEquipManager::GetSingleton();
            auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(selectedForm.load(std::memory_order_relaxed));
            if (!manager || !object) {
                failure();
                return;
            }
            Record(equip ? 'e' : 'u', object->GetFormID(), 0);
            if (equip) {
                manager->EquipObject(player, object, nullptr, 1, nullptr, false, false, false, true);
            } else {
                manager->UnequipObject(player, object, nullptr, 1, nullptr, false, false, false, true);
            }
            const std::lock_guard guard(trial->lock);
            ++trial->actions;
        }

        const auto now = Clock::now();
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{ new ProbeCallback(trial, now) };
        if (!vm->DispatchStaticCall("Utility", "Wait", RE::MakeFunctionArguments(0.01f), callback)) {
            failure();
            return;
        }
        {
            const std::lock_guard guard(trial->lock);
            ++trial->requested;
        }
        if (checkPlayer) {
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> check{ new PlayerCallback(trial) };
            if (!vm->DispatchStaticCall("Game", "GetPlayer", RE::MakeFunctionArguments(), check)) {
                failure();
            }
        }
    }

    bool RunExteriorProbes(const std::shared_ptr<Trial>& trial, std::stop_token stop)
    {
        const auto deadline = Clock::now() + 30s;
        while (Clock::now() < deadline && !stop.stop_requested()) {
            {
                const std::lock_guard guard(trial->lock);
                if (trial->responses.size() == probeCount) {
                    return true;
                }
            }
            if (!trial->probePending.exchange(true)) {
                const std::lock_guard guard(trial->lock);
                if (trial->requested < probeCount) {
                    SKSE::GetTaskInterface()->AddTask([trial] {
                        Tick(trial, false, false, false);
                        trial->probePending.store(false);
                    });
                } else {
                    trial->probePending.store(false);
                }
            }
            std::this_thread::sleep_for(spacing);
        }
        return false;
    }

    void SelectEquipment()
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (vm) {
            profileActive.store(vm->profilePolicy != nullptr);
        }
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (comparePlayer && vm) {
                const auto* policy = vm->GetObjectHandlePolicy();
                RE::BSTSmartPointer<RE::BSScript::Object> bound;
                if (policy) {
                    constexpr auto actorType = static_cast<RE::VMTypeID>(RE::Actor::FORMTYPE);
                    const auto handle = policy->GetHandleForObject(actorType, player);
                    playerBound.store(handle != policy->EmptyHandle() && vm->FindBoundObject(handle, "Actor", bound) &&
                        bound && bound->Resolve(actorType) == player);
                }
            }
            auto* weapon = player->GetEquippedObject(false);
            auto* armor = player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody, true);
            if (weapon && weapon->As<RE::TESObjectWEAP>()) {
                selectedForm.store(weapon->GetFormID());
            } else if (armor) {
                selectedForm.store(armor->GetFormID());
            }
        }
    }

    void WriteTrace(std::ofstream& out, unsigned trial)
    {
        const std::lock_guard guard(traceLock);
        for (const auto& event : events) {
            out << trial << ',' << event.qpc << ',' << event.kind << ',' << event.stack << ',' << event.depth;
            for (const auto& name : event.names) {
                out << ',' << std::quoted(name.data());
            }
            out << ',' << event.thread << ',' << event.detail << '\n';
        }
        out.flush();
    }

    void Run(std::stop_token stop)
    {
        std::this_thread::sleep_for(15s);
        SKSE::GetTaskInterface()->AddTask(SelectEquipment);
        std::this_thread::sleep_for(1s);
        if (!profileActive.load()) {
            SKSE::log::error("gameplay trace aborted: the VM has no profiling policy; enable bEnableProfiling in the test profile");
            return;
        }
        if (comparePlayer && !playerBound.load()) {
            SKSE::log::error("gameplay trace aborted: the player has no existing Actor VM binding");
            return;
        }
        const auto form = selectedForm.load();
        if (!form && !profileCells) {
            SKSE::log::error("gameplay trace aborted: no equipped weapon or body armor");
            return;
        }
        const auto logPath = SKSE::log::log_directory();
        if (!logPath) {
            SKSE::log::error("gameplay trace aborted: log directory missing");
            return;
        }
        std::ofstream trace(*logPath / "APBGameplayTrace.csv");
        if (!trace) {
            SKSE::log::error("gameplay trace aborted: could not open trace output");
            return;
        }
        trace << "trial,qpc,kind,stack,depth,name1,name2,name3,thread,detail\n";
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        SKSE::log::info("gameplay trace start form={:08X} qpc_hz={} single_change={}", form, frequency.QuadPart, singleChange);
        events.reserve(observeJobBatch ? 2 * maxTraceEvents : maxTraceEvents);

        if ((replicateContinuation || observeGetAtReturn || observeNativeDrain) && !AwaitStableCadence(stop, 10, 60s)) {
            SKSE::log::error("gameplay trace aborted: Save93 rendering did not stabilize before travel");
            return;
        }
        const auto trialCount = profileCells && !replicateContinuation ? 4 : sequence.size();
        for (unsigned index = 0; index < trialCount && !stop.stop_requested(); ++index) {
            const bool selected = sequence[(index + (reverseContinuation ? sequence.size() / 2 : 0)) % sequence.size()];
            const bool workload = !profileCells && (comparePlayer || compareCatchUp || compareHeadroom || traceResume || selected);
            const bool enabled = compareContinuation || compareNativeDrain ? selected :
                (comparePlayer || compareCatchUp || compareHeadroom || compareRetry || compareEntry) && selected;
            auto trial = std::make_shared<Trial>();
            if (compareRetry || compareEntry) {
                ResumeTrace::BeginTrial(enabled);
            }
            if (compareContinuation) {
                Continuation::BeginTrial(enabled);
            }
            if (compareNativeDrain) {
                NativeDrainTrace::BeginTrial(enabled);
            }
            if (stagedExterior) {
                auto staging = std::make_shared<Trial>();
                if (!AwaitArrival(staging, 0, stop) || staging->destinationCell != 0x0001605E) {
                    SKSE::log::error("gameplay trace aborted: staging did not reach the Bannered Mare");
                    return;
                }
                if (!AwaitStableCadence(stop, 3, (replicateContinuation || observeGetAtReturn || observeNativeDrain) ? 60s : 35s)) {
                    SKSE::log::error("gameplay trace aborted: Bannered Mare rendering did not stabilize");
                    return;
                }
            }
            {
                const std::lock_guard guard(traceLock);
                events.clear();
                droppedEvents = 0;
                capture.store(true);
            }
            NativePacing::ResetCounts();
            promote.store(comparePlayer && enabled);
            (void)VMCatchUp::ReadAndResetStats();
            if (compareCatchUpTarget) {
                VMCatchUp::SetTargetHz(enabled ? 60.0 : 50.0);
            }
            if (compareCatchUpDirect) {
                VMCatchUp::SetDirectJudgement(enabled);
            }
            if (compareCatchUpExclusions) {
                VMCatchUp::SetExclusionList(enabled);
            }
            if (compareCatchUpHoldCalls) {
                VMCatchUp::SetHoldChangeCalls(enabled);
            }
            if (compareCatchUpGetter) {
                VMCatchUp::SetPropertyReads(enabled);
            }
            if (compareCatchUpSkipFront) {
                VMCatchUp::SetSkipBlocked(enabled);
            }
            if (compareCatchUpReadRule) {
                VMCatchUp::SetReadRule(enabled ? VMCatchUp::ReadRule::kReviewed : VMCatchUp::ReadRule::kNames);
            }
            catchUpEnabled.store(compareCatchUp && (enabled || CatchUpAlwaysOn()));
            if (compareHeadroom) {
                QueueHeadroom::BeginTrial(enabled);
            }
            SKSE::log::info("gameplay window trial={} equip={} enabled={} cells={} qpc={}", index + 1, workload, enabled, profileCells, QPC());
            ForegroundSampler foreground;
            if (profileCells) {
                if (!AwaitArrival(trial, stagedExterior ? 1 : index, stop)) {
                    capture.store(false);
                    SKSE::log::error("gameplay trace aborted: no cell arrival within the observation window");
                    return;
                }
                // Separate loading and its task-queue burst from the response probes.
                std::this_thread::sleep_for(1s);
                Record('v', 0, 0);
            }
            const auto start = Clock::now();
            if (stagedExterior && !RunExteriorProbes(trial, stop)) {
                Continuation::Arm(false);
                NativeDrainTrace::Arm(false);
                capture.store(false);
                SKSE::log::error("gameplay trace aborted: exterior response callbacks timed out");
                return;
            }
            for (unsigned i = 0; !stagedExterior && i < probeCount; ++i) {
                std::this_thread::sleep_until(start + spacing * i);
                if (stop.stop_requested()) {
                    Continuation::Arm(false);
                    capture.store(false);
                    promote.store(false);
                    catchUpEnabled.store(false);
                    NativePacing::Pause();
                    if (compareHeadroom) {
                        QueueHeadroom::EndTrial();
                    }
                    return;
                }
                SKSE::GetTaskInterface()->AddTask([trial, i, workload] {
                    Tick(trial, workload && IsChangeTick(i), IsEquipTick(i), comparePlayer && i == probeCount - 2);
                });
            }
            if (!stagedExterior) {
                std::this_thread::sleep_until(start + spacing * probeCount);
            }
            Continuation::Arm(false);
            NativeDrainTrace::Arm(false);
            promote.store(false);
            catchUpEnabled.store(false);
            const auto catchUp = VMCatchUp::ReadAndResetStats();
            NativePacing::Pause();
            if (compareHeadroom && !QueueHeadroom::EndTrial()) {
                capture.store(false);
                SKSE::log::error("gameplay trace aborted: no queue samples or headroom mode did not take effect");
                return;
            }
            capture.store(false);
            SKSE::log::info("gameplay end trial={} qpc={} foreground_pct={:.1f}", index + 1, QPC(), foreground.Stop());
            WriteTrace(trace, index + 1);
            if (traceResume && !std::ranges::all_of(std::string_view("MNRTGX"), [](char kind) {
                    return std::ranges::any_of(events, [kind](const TraceEvent& event) { return event.kind == kind; });
                })) {
                SKSE::log::error("gameplay trace aborted: a native dispatch or resume marker was missing");
                return;
            }
            {
                const std::lock_guard guard(trial->lock);
                const auto counts = NativePacing::ReadCounts();
                if (comparePlayer && (trial->playerID != 0x14 || (counts.promoted > 0) != enabled ||
                    counts.peak > NativePacing::callsPerUpdate)) {
                    SKSE::log::error("gameplay trace aborted: trial={} player={:08X} enabled={} promoted={} peak={}",
                        index + 1, trial->playerID, enabled, counts.promoted, counts.peak);
                    return;
                }
                const bool reachedCell = stagedExterior ? trial->destinationCell == 0x0001A27F : trial->destinationCell != 0;
                const bool complete = profileCells ? reachedCell && trial->requested >= 8 : trial->requested == probeCount;
                const auto expectedActions = profileCells ? 1u : workload ? ChangesPerTrial() : 0u;
                if (trial->failures || !complete || trial->responses.size() != trial->requested ||
                    trial->actions != expectedActions || droppedEvents) {
                    SKSE::log::error("gameplay trace aborted: trial={} requested={} responses={} actions={} failures={} dropped={}",
                        index + 1, trial->requested, trial->responses.size(), trial->actions, trial->failures, droppedEvents);
                    return;
                }
                if (compareCatchUp) {
                    // The gate must hold the feature off in A trials and open it in B trials. Frames at or
                    // above the target rate legitimately run no extra cycles, so only slow frames require them.
                    // Any guard violation means an extra cycle ran a call that was queued during the same frame.
                    const bool active = enabled || CatchUpAlwaysOn();
                    // Zero extra cycles is a valid result: the carried mode only runs left-over calls.
                    const bool gateWrong = active ? catchUp.frames == 0 : catchUp.frames != 0 || catchUp.cycles != 0;
                    if (catchUp.guardViolations != 0 || gateWrong) {
                        SKSE::log::error("gameplay trace aborted: catch-up trial={} enabled={} frames={} cycles={} guard_violations={}",
                            index + 1, enabled, catchUp.frames, catchUp.cycles, catchUp.guardViolations);
                        return;
                    }
                    // The skip rule works in accelerate only; in carried the setting changes nothing.
                    const bool skipBlocked = active && installedAccelerate && (compareCatchUpSkipFront ? enabled : skipBlockedWholeRun);
                    SKSE::log::info("catch-up trial={} enabled={} skip_blocked={} target_hz={:.0f} frames={} slow={} engaged={} avg_frame_ms={:.3f} cycles={} with_tasklets={} "
                                    "extra_calls={} leftover_tasklets={} "
                                    "idle_stops={} time_stops={} blocked_stops={} blocked_skips={} guard_violations={} direct_changes={} change_call_holds={} front_skips={} order_rebuilds={} frozen_stops={} held_frames={} overrun_pauses={} extra_ms_total={:.3f} "
                                    "extra_ms_per_frame={:.4f} max_frame_extra_ms={:.3f} max_update_ms={:.3f} max_batch_ms={:.3f}",
                        index + 1, enabled, skipBlocked, compareCatchUpTarget ? (enabled ? 60.0 : 50.0) : installedTargetHz, catchUp.frames, catchUp.slowFrames, catchUp.engagedFrames,
                        catchUp.frames ? catchUp.intervalMS / static_cast<double>(catchUp.frames) : 0.0,
                        catchUp.cycles, catchUp.taskletCycles, catchUp.extraCalls, catchUp.leftoverTasklets,
                        catchUp.idleStops, catchUp.timeStops, catchUp.blockedStops, catchUp.blockedSkips, catchUp.guardViolations,
                        catchUp.directChanges, catchUp.changeCallHolds, catchUp.frontSkips, catchUp.orderRebuilds, catchUp.frozenStops,
                        catchUp.heldFrames, catchUp.overrunPauses,
                        catchUp.extraMS, catchUp.frames ? catchUp.extraMS / static_cast<double>(catchUp.frames) : 0.0,
                        catchUp.maxFrameExtraMS, catchUp.maxUpdateMS, catchUp.maxBatchMS);
                }
                if (profileCells) {
                    SKSE::log::info("cell change {:08X} -> {:08X} responses={}", trial->sourceCell, trial->destinationCell, trial->requested);
                }
                std::string samples;
                for (const auto value : trial->responses) {
                    samples += std::format("{:.3f},", value);
                }
                samples.pop_back();
                SKSE::log::info("gameplay result trial={} equip={} enabled={} actions={} events={} player={:08X} promoted={} peak={} probes={}",
                    index + 1, workload, enabled, trial->actions, events.size(), trial->playerID, counts.promoted, counts.peak, samples);
            }
            std::this_thread::sleep_for(2s);
        }
        if (censusCatchUp) {
            LogCensus();
        }
        if (!stop.stop_requested()) {
            SKSE::log::info("gameplay trace complete");
        }
    }

    void RunSoak(std::stop_token stop)
    {
        constexpr unsigned soakCycles = 8;
        std::this_thread::sleep_for(15s);
        SKSE::GetTaskInterface()->AddTask(SelectEquipment);
        std::this_thread::sleep_for(1s);
        const auto form = selectedForm.load();
        SKSE::log::info("catch-up soak start enabled={} form={:08X}", soakEnabled, form);
        catchUpEnabled.store(soakEnabled);
        (void)VMCatchUp::ReadAndResetStats();
        for (unsigned cycle = 0; cycle < soakCycles && !stop.stop_requested(); ++cycle) {
            auto trial = std::make_shared<Trial>();
            if (!AwaitArrival(trial, cycle, stop)) {
                catchUpEnabled.store(false);
                SKSE::log::error("catch-up soak aborted: cycle={} did not arrive", cycle + 1);
                return;
            }
            // Past the plugin's 10 s hold after a loading screen, so the round runs with catch-up active.
            std::this_thread::sleep_for(11s);
            if (soakJB1) {
                SKSE::GetTaskInterface()->AddTask([cycle] { LogJB1State(cycle + 1, "after_arrival"); });
            }
            ForegroundSampler foreground;
            const auto start = Clock::now();
            for (unsigned i = 0; i < probeCount && !stop.stop_requested(); ++i) {
                std::this_thread::sleep_until(start + spacing * i);
                SKSE::GetTaskInterface()->AddTask([trial, i, form] {
                    Tick(trial, form != 0 && i < changes, (i % 2) != 0, false);
                });
            }
            std::this_thread::sleep_until(start + spacing * probeCount + 3s);
            const double foregroundPct = foreground.Stop();
            const auto stats = VMCatchUp::ReadAndResetStats();
            std::size_t responses{};
            unsigned requested{};
            unsigned failures{};
            {
                const std::lock_guard guard(trial->lock);
                responses = trial->responses.size();
                requested = trial->requested;
                failures = trial->failures;
            }
            SKSE::log::info("catch-up soak cycle={} cell={:08X} requested={} responses={} failures={} frames={} slow={} engaged={} "
                            "cycles={} with_tasklets={} extra_calls={} leftover_tasklets={} blocked_stops={} blocked_skips={} guard_violations={} direct_changes={} overrun_pauses={} gate_closes={} max_frame_extra_ms={:.3f} "
                            "max_update_ms={:.3f} max_batch_ms={:.3f} slowest_batch_tasklets={} max_batch_tasklets={} foreground_pct={:.1f}",
                cycle + 1, trial->destinationCell, requested, responses, failures, stats.frames, stats.slowFrames, stats.engagedFrames, stats.cycles,
                stats.taskletCycles, stats.extraCalls, stats.leftoverTasklets, stats.blockedStops, stats.blockedSkips, stats.guardViolations, stats.directChanges, stats.overrunPauses,
                stats.gateCloses, stats.maxFrameExtraMS,
                stats.maxUpdateMS, stats.maxBatchMS, stats.slowestBatchTasklets, stats.maxBatchTasklets, foregroundPct);
            if (stats.guardViolations != 0 || (!soakEnabled && stats.cycles != 0)) {
                catchUpEnabled.store(false);
                SKSE::log::error("catch-up soak aborted: cycle={} guard_violations={} cycles={}", cycle + 1, stats.guardViolations, stats.cycles);
                return;
            }
            if (soakJB1 && cycle + 1 < soakCycles) {
                // Started 4 s before the next trip, with catch-up in its normal state for this run.
                SKSE::GetTaskInterface()->AddTask([cycle] {
                    // TESQuest::Start is the engine call behind the console's startquest; it fails
                    // when a non-optional alias finds no matching reference.
                    auto* data = RE::TESDataHandler::GetSingleton();
                    std::string results;
                    for (const auto& probe : jb1Quests) {
                        if (!probe.start) {
                            continue;
                        }
                        auto* quest = data ? data->LookupForm<RE::TESQuest>(probe.id, "Chatty NPCs.esp") : nullptr;
                        const bool started = quest && quest->Start();
                        results += std::format(" {}={}", probe.editorID, started);
                    }
                    SKSE::log::info("jb1 start cycle={}{}", cycle + 2, results);
                });
                std::this_thread::sleep_for(500ms);
                SKSE::GetTaskInterface()->AddTask([cycle] { LogJB1State(cycle + 2, "after_start"); });
                std::this_thread::sleep_for(2500ms);
                SKSE::GetTaskInterface()->AddTask([cycle] { LogJB1State(cycle + 2, "before_travel"); });
                std::this_thread::sleep_for(1s);
            }
        }
        if (soakJB1) {
            std::this_thread::sleep_for(5s);
            SKSE::GetTaskInterface()->AddTask([] { LogJB1State(soakCycles, "final"); });
            std::this_thread::sleep_for(1s);
        }
        catchUpEnabled.store(false);
        if (!stop.stop_requested()) {
            SKSE::log::info("catch-up soak complete");
        }
    }

    std::jthread worker;

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        if (message->type == SKSE::MessagingInterface::kPostPostLoad) {
            installed = InstallProfiler() && (!comparePlayer || InstallPlayerExperiment()) &&
                (!compareCatchUp || !installCatchUpModule || InstallCatchUp()) &&
                (!compareHeadroom || QueueHeadroom::Install()) &&
                (!traceResume || ResumeTrace::Install([] { return capture.load(std::memory_order_relaxed); }, Record,
                    compareRetry, compareEntry, observeGetAtReturn, observeNativeStages, observeReleaseSource)) &&
                (!compareContinuation || Continuation::Install(Record, replicateContinuation)) &&
                (!observeNativeDrain || NativeDrainTrace::Install([] { return capture.load(std::memory_order_relaxed); }, Record,
                    compareNativeDrain, observeNativeStages, observeTaskletFinish, observeJobBatch, observeReleaseSource));
            SKSE::log::info("gameplay trace profiler installed={}", installed);
            if (!installed) {
                SKSE::log::error("gameplay trace aborted: a required hook did not match the verified runtime");
            }
        } else if (message->type == SKSE::MessagingInterface::kPreLoadGame && compareHeadroom) {
            QueueHeadroom::ObserveGameplay(false);
        } else if (message->type == SKSE::MessagingInterface::kPostLoadGame && message->data && installed &&
            !started.exchange(true)) {
            if (compareCatchUp) {
                VMCatchUp::NotifyGameLoaded();
            }
            RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(&equipObserver);
            if (compareHeadroom) {
                if (auto* ui = RE::UI::GetSingleton()) {
                    ui->AddEventSink(&menuObserver);
                }
            }
            if (soakCatchUp) {
                if (auto* ui = RE::UI::GetSingleton()) {
                    ui->AddEventSink(&menuObserver);
                }
                MenuWatch::Sync();
                worker = std::jthread(RunSoak);
            } else {
                worker = std::jthread(Run);
            }
        }
    }
}

SKSE_PLUGIN_VERSION = [] {
    SKSE::PluginVersionData info{};
    info.PluginName("APBGameplayTrace");
    info.PluginVersion({ 0, 1, 0, 0 });
    info.CompatibleVersions({ runtime });
    info.UsesUpdatedStructs();
    return info;
}();

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    if (skse->RuntimeVersion() != runtime) {
        return false;
    }
    const char* active = std::getenv("APB_GAMEPLAY_TRACE");
    const auto mode = active ? std::string_view(active) : std::string_view{};
    comparePlayer = mode == "ab";
    compareCatchUpTarget = mode == "catchup-target";
    compareCatchUpDirect = mode == "catchup-direct";
    compareCatchUpExclusions = mode == "catchup-exclusions";
    compareCatchUpHoldCalls = mode == "catchup-holdcalls";
    compareCatchUpReadRule = mode == "catchup-readrule";
    compareCatchUpGetter = mode == "catchup-getter";
    compareCatchUpSkipFront = mode == "catchup-skipfront";
    if (const char* single = std::getenv("APB_EQUIP_SINGLE")) {
        singleChange = std::string_view(single) == "1";
    }
    if (const char* skip = std::getenv("APB_CATCHUP_SKIP")) {
        // Whole-run setting for the other modes, e.g. catch-up off against catch-up on with skipping.
        skipBlockedWholeRun = std::string_view(skip) == "1";
        VMCatchUp::SetSkipBlocked(skipBlockedWholeRun);
    }
    if (compareCatchUpExclusions) {
        // The exclusion list is part of the name rule.
        VMCatchUp::SetReadRule(VMCatchUp::ReadRule::kNames);
    }
    censusCatchUp = mode == "catchup-census";
    if (censusCatchUp) {
        VMCatchUp::SetCensus(true);
    }
    if (const char* direct = std::getenv("APB_CATCHUP_DIRECT")) {
        VMCatchUp::SetDirectJudgement(std::string_view(direct) != "0");
    }
    soakCatchUp = mode == "catchup-soak";
    if (const char* jb1 = std::getenv("APB_SOAK_JB1")) {
        soakJB1 = std::string_view(jb1) == "1";
    }
    if (const char* on = std::getenv("APB_CATCHUP_ON")) {
        // "dry": the gate opens and every check runs, but no extra cycle executes.
        soakEnabled = std::string_view(on) == "1" || std::string_view(on) == "dry";
        installCatchUpModule = std::string_view(on) != "none";
        VMCatchUp::SetCyclesAllowed(std::string_view(on) != "dry");
    }
    compareCatchUp = mode == "catchup-ab" || CatchUpAlwaysOn() || soakCatchUp;
    compareHeadroom = mode == "headroom";
    compareRetry = mode == "retry";
    compareEntry = mode == "entry-retry";
    reverseContinuation = mode == "continuation-replication-reverse";
    replicateContinuation = mode == "continuation-replication" || reverseContinuation;
    compareContinuation = mode == "continuation-ab" || replicateContinuation;
    observeGetAtReturn = mode == "getat-return-observe";
    observeReleaseSource = mode == "getworldspace-release-observe";
    observeJobBatch = mode == "getworldspace-batch-observe" || observeReleaseSource;
    observeTaskletFinish = mode == "getworldspace-finish-observe" || observeJobBatch;
    observeNativeStages = mode == "getworldspace-stage-observe" || observeTaskletFinish;
    compareNativeDrain = mode == "getworldspace-drain-ab";
    observeNativeDrain = mode == "getworldspace-drain-observe" || compareNativeDrain || observeNativeStages;
    stagedExterior = compareContinuation || observeGetAtReturn || observeNativeDrain;
    profileCells = mode == "scene-profile" || stagedExterior;
    traceResume = mode == "resume" || compareRetry || compareEntry || profileCells;
    SKSE::Init(skse, { .logName = "APBGameplayTrace", .trampoline = comparePlayer || compareCatchUp || compareHeadroom || traceResume,
        .trampolineSize = observeJobBatch ? 640u : observeTaskletFinish ? 512u : traceResume ? 320u : compareCatchUp ? 288u : 64u });
    return (mode != "1" && !comparePlayer && !compareCatchUp && !compareHeadroom && !traceResume) ||
        SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
}
