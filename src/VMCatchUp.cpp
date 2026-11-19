#include "VMCatchUp.h"

#include "CatchUpPolicy.h"
#include "NativeClassification.h"
#include "QueueOrder.h"
#include "SlowFrameReport.h"

#include "RE/F/FunctionMessage.h"

#include <algorithm>
#include <intrin.h>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using JobList = RE::BSJobs::JobList;
    using VirtualMachine = RE::BSScript::Internal::VirtualMachine;
    using JobsFn = void(JobList*);
    using SubmitFn = void(JobList*, JobList*, std::uint64_t);

    // AE 1.6.1170 "VM update" job (ID 39074) from +0x50: RDI holds the gameplay job list (35404(0))
    // and RBX the VM. It calls TasksToJobs inside a scope object whose constructor (69360,
    // `mov rax, rcx; ret`) and destructor (69361, `ret 0`) do nothing, then submits, waits for and
    // resets the job list. Extra cycles therefore call TasksToJobs, submit, wait and reset directly.
    constexpr std::size_t jobTailOffset = 0x50;
    constexpr std::size_t resetCallOffset = 0x94;
    constexpr std::array<std::uint8_t, 73> jobTail{
        0x48, 0x8B, 0xF8, 0x48, 0x8B, 0xD0, 0x48, 0x8D, 0x4C, 0x24, 0x48, 0xE8, 0x40, 0x3D, 0x62, 0x00,
        0x90, 0x4C, 0x8B, 0x03, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCB, 0x41, 0xFF, 0x90, 0x90, 0x00, 0x00,
        0x00, 0x90, 0x48, 0x8D, 0x4C, 0x24, 0x48, 0xE8, 0x34, 0x3D, 0x62, 0x00, 0x45, 0x33, 0xC0, 0x33,
        0xD2, 0x48, 0x8B, 0xCF, 0xE8, 0x97, 0x3A, 0x62, 0x00, 0x48, 0x8B, 0xCF, 0xE8, 0xAF, 0x3A, 0x62,
        0x00, 0x48, 0x8B, 0xCF, 0xE8, 0x37, 0x3B, 0x62, 0x00
    };
    static_assert(jobTailOffset + jobTail.size() == resetCallOffset + 5);
    // E8 calls inside jobTail: the empty scope pair around TasksToJobs, submit, wait, reset.
    constexpr std::array<std::size_t, 5> jobTailCalls{ 11, 39, 52, 60, 68 };

    // Compares code exactly, except the targets of the listed E8 calls. Another plugin that already
    // hooked one of those calls with SKSE's write_call (for example Papyrus Tweaks NG's argument
    // log) keeps working: our hook calls whatever target it finds, which then calls the stock code.
    template <std::size_t N, std::size_t M>
    bool MatchesCode(std::uintptr_t address, const std::array<std::uint8_t, N>& code,
        const std::array<std::size_t, M>& calls, bool& chained)
    {
        const auto* live = reinterpret_cast<const std::uint8_t*>(address);
        for (std::size_t i = 0; i < N; ++i) {
            const bool target = std::ranges::any_of(calls, [i](std::size_t call) { return i > call && i <= call + 4; });
            if (live[i] == code[i]) {
                continue;
            }
            if (!target) {
                return false;
            }
            chained = true;
        }
        return true;
    }

    // SkyrimVM::UpdateGame gives VirtualMachine::Update 0.9 of the 0.9 game share of
    // fUpdateBudgetMS when its event processing finishes immediately, never below 0.1 ms.
    constexpr float stockUpdateShare = 0.81f;
    constexpr float minimumUpdateMS = 0.1f;
    constexpr float maximumUpdateMS = 10.0f;
    // A loaded save runs its OnInit/OnPlayerLoadGame burst first; single calls in it took ~40 ms.
    constexpr auto settleAfterLoad = std::chrono::seconds(10);
    // A frame whose extra work overran the limit this much pauses catch-up so a burst cannot repeat it.
    constexpr double overrunFactor = 2.0;
    constexpr auto overrunPause = std::chrono::seconds(3);

    REL::Relocation<JobsFn*> originalReset;
    JobsFn* submitAddress{};
    JobsFn* waitAddress{};
    const float* updateBudgetMS{};

    std::mutex stateLock;
    CatchUpPolicy policy;
    VMCatchUp::Settings settings;
    VMCatchUp::GateFn* gameplayGate{};
    VMCatchUp::Stats stats{};
    Clock::time_point previousCycle{};
    // Since when RunExtraCycles has found the gate closed, in how many VM updates, and whether
    // ReportClosedGate has said so.
    Clock::time_point closedSince{};
    std::uint64_t closedUpdates{};
    bool closedReported{};
    Clock::time_point nextLog{};
    Clock::time_point settleUntil{};
    Clock::time_point pausedUntil{};
    // Written by the load message without stateLock, which the hook holds while it waits for jobs.
    std::atomic<Clock::rep> loadedAt{ 0 };
    Clock::rep appliedLoad{ 0 };
    std::atomic_bool cyclesAllowed{ true };
    bool installed{};

    // Decides which queued script calls an extra cycle may run. Two modes:
    //
    // carried: only calls still waiting in the native queue when this frame's stock VM update
    //   finished, in queue order. A call queued during this frame's tasklets waits for the next
    //   stock update, so a script's delayed calls stay at least one frame apart. Such a call now runs
    //   after this frame's tasklets instead of in the next frame's update, so the order in which it
    //   observes other scripts' work can still differ from the stock game.
    // accelerate (experimental): also a script's next call in the same frame when every native it ran
    //   this frame left game state alone (the reviewed reads of NativeClassification.h, auto-property
    //   getters, and side-effect-free helpers) and it existed before this frame. Natives are judged both
    //   when the queue runs them and when a tasklet calls them directly. This removes the game update
    //   between two of its calls, which some scripts may depend on.
    //
    // In both modes the front of the queue is never skipped: an extra cycle starts only when the front
    // call may run, and stops when a call that may not run reaches the front. Extra cycles skip the
    // suspended-stack retry pass. Hooks only observe or act in a frame that began with the gate open
    // and the smoothed frame rate below the engage rate; otherwise they pass every call straight through.
    //
    // The rules above cover queued calls only. The tasklet half of an extra cycle is the stock
    // TasksToJobs, which runs every tasklet in vmTasks: the scripts the cycle's calls resumed, and
    // also tasklets the engine put back when they reached the per-run op limit (ID 105176 re-adds
    // the tasklet to vmTasks). Those continue one more slice in the same frame whatever their script
    // did; their next queued call still waits under the mode's rule.
    namespace Chain
    {
        using Stack = RE::BSScript::Stack;
        using Function = RE::BSScript::IFunction;
        using Message = RE::BSScript::Internal::FunctionMessage;
        using QueryPtr = RE::BSTSmartPointer<RE::BSScript::Internal::IFuncCallQuery>;
        using FunctionPtr = RE::BSTSmartPointer<Function>;
        using DispatchFn = std::uint64_t(VirtualMachine*, Stack*, const QueryPtr*, bool);
        using ReturnFn = std::uint64_t(VirtualMachine*, Stack*, std::uint64_t, std::uint64_t);
        using PrepareFn = std::uint64_t(FunctionPtr*, void*, void*, std::uint64_t);
        using AttemptFn = std::uint64_t(VirtualMachine*, Stack*, void*, bool);
        using QueueFn = std::uint64_t(VirtualMachine*, Stack*, void*);
        using PopFn = bool(VirtualMachine*, Message*);
        using RetryPassFn = void(VirtualMachine*, float, std::uint32_t*);
        using UpdateGameFn = void(RE::SkyrimVM*, float);
        using Mode = VMCatchUp::Mode;

        struct Site
        {
            std::size_t offset;
            std::size_t call;  // offset of the E8 call inside the verified bytes
        };

        // "VM update" job (ID 39074) +0x14: calls SkyrimVM::UpdateGame, which runs the stock
        // VirtualMachine::Update. The job then turns the resumed scripts into tasklet jobs.
        constexpr Site updateGame{ 0x14, 10 };
        constexpr std::array<std::uint8_t, 15> updateGameCode{
            0x0F, 0x57, 0xC9, 0x48, 0x8B, 0x0D, 0x82, 0x8D, 0xA2, 0x01, 0xE8, 0x0D, 0xCE, 0x2E, 0x00
        };
        // AttemptFunctionCall (ID 104853) +0x63D: every call that runs now reaches this argument
        // setup: the drain and the retry pass, and tasklet calls whose function may run on a tasklet
        // (+0x60B..0x638 queues the others through 104848 instead). RCX points at the IFunction
        // smart pointer.
        constexpr Site prepare{ 0x63D, 0x13 };
        constexpr std::array<std::uint8_t, 24> prepareCode{
            0x41, 0xB9, 0x00, 0x04, 0x00, 0x00, 0x4C, 0x8D, 0x45, 0x10, 0x48, 0x8D, 0x55, 0x98, 0x48, 0x8D,
            0x4C, 0x24, 0x50, 0xE8, 0x3B, 0x33, 0x01, 0x00
        };
        // The two tasklet callers of AttemptFunctionCall pass the calling stack in RDX: the code
        // tasklet's call opcode (ID 105176 +0x82F) and its resume path (ID 105204 +0x56).
        constexpr Site taskletCall{ 0x81C, 19 };
        constexpr std::array<std::uint8_t, 24> taskletCallCode{
            0x45, 0x0F, 0xB6, 0xCE, 0x4C, 0x8D, 0x85, 0x00, 0x09, 0x00, 0x00, 0x48, 0x8B, 0x56, 0x10, 0x48,
            0x8B, 0x4E, 0x18, 0xE8, 0xEC, 0xB4, 0xFE, 0xFF
        };
        // Test builds only (Settings::observeQueuedCalls). AttemptFunctionCall +0x62A: a tasklet's
        // call to a function that may not run on a tasklet is queued here through ID 104848
        // (RCX = VM, RDX = stack, R8 = query). The function the call resolved to is in the smart
        // pointer at [rsp+0x50] of AttemptFunctionCall's frame: +0x618 loads it from there for the
        // tasklet check, and +0x63D passes that address on as the argument setup's first argument.
        constexpr Site queueCall{ 0x62A, 9 };
        constexpr std::array<std::uint8_t, 14> queueCallCode{
            0x4D, 0x8B, 0xC4, 0x48, 0x8B, 0xD3, 0x48, 0x8B, 0xCE, 0xE8, 0x38, 0xEF, 0xFF, 0xFF
        };
        constexpr std::size_t resolvedFunctionOffset = 0x50;
        constexpr Site resumeCall{ 0x46, 16 };
        constexpr std::array<std::uint8_t, 21> resumeCallCode{
            0x41, 0xB1, 0x01, 0x4C, 0x8D, 0x44, 0x24, 0x50, 0x48, 0x8B, 0x51, 0x10, 0x48, 0x8B, 0x49, 0x18,
            0xE8, 0x85, 0x73, 0xFE, 0xFF
        };
        // Native-call drain (ID 104857): both pops run while the drain holds funcQueueLock, and the
        // message is dispatched after the lock is released (calls to 104853, returns to 104854).
        // Pop (104879) takes the head of the linked queue (105142 -> 105068) and, when that is
        // empty, the last overflow entry; it returns false without touching the queue or the waiting
        // count when both are empty, and the drain then ends (104857 +0x134 / +0x1F9). Every
        // gameplay push (104848, 104849) holds funcQueueLock, so under it the front read here is the
        // entry the next pop takes.
        constexpr Site firstPop{ 0xF2, 8 };
        constexpr std::array<std::uint8_t, 13> firstPopCode{
            0x48, 0x8D, 0x54, 0x24, 0x28, 0x48, 0x8B, 0xCE, 0xE8, 0x41, 0x22, 0x00, 0x00
        };
        constexpr Site nextPop{ 0x1B7, 8 };
        constexpr std::array<std::uint8_t, 13> nextPopCode{
            0x48, 0x8D, 0x54, 0x24, 0x28, 0x48, 0x8B, 0xCE, 0xE8, 0x7C, 0x21, 0x00, 0x00
        };
        constexpr Site drainDispatch{ 0x17E, 16 };
        constexpr std::array<std::uint8_t, 21> drainDispatchCode{
            0x45, 0x33, 0xC9, 0x4C, 0x8D, 0x44, 0x24, 0x38, 0x48, 0x8B, 0x54, 0x24, 0x30, 0x48, 0x8B, 0xCE,
            0xE8, 0xBD, 0xEB, 0xFF, 0xFF
        };
        constexpr Site drainReturn{ 0x169, 14 };
        constexpr std::array<std::uint8_t, 19> drainReturnCode{
            0x45, 0x33, 0xC9, 0x41, 0xB0, 0x01, 0x48, 0x8B, 0x54, 0x24, 0x30, 0x48, 0x8B, 0xCE, 0xE8, 0xA4,
            0xF6, 0xFF, 0xFF
        };
        // Suspended-stack retry pass (ID 104858) +0x441: retries a suspended call.
        constexpr Site retryDispatch{ 0x441, 14 };
        constexpr std::array<std::uint8_t, 19> retryDispatchCode{
            0x45, 0x33, 0xC9, 0x4C, 0x8D, 0x44, 0x24, 0x50, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCE, 0xE8, 0x7C,
            0xE6, 0xFF, 0xFF
        };
        // VirtualMachine::Update (ID 104767) +0x104: calls the retry pass with the count output in R8.
        constexpr Site retryPass{ 0x104, 10 };
        constexpr std::array<std::uint8_t, 15> retryPassCode{
            0x4C, 0x8D, 0x45, 0x67, 0x0F, 0x28, 0xCF, 0x48, 0x8B, 0xCE, 0xE8, 0x7D, 0xB5, 0x00, 0x00
        };
        // The retry pass reports this sum of suspended entries before it resumes anything (104858 +0x2D9).
        constexpr std::size_t queueEntriesOffset = 0x810;
        constexpr std::size_t arraySizeOffset = 0x10;

        REL::Relocation<UpdateGameFn*> originalUpdateGame;
        REL::Relocation<PrepareFn*> originalPrepare;
        REL::Relocation<PopFn*> originalFirstPop;
        REL::Relocation<PopFn*> originalNextPop;
        REL::Relocation<DispatchFn*> originalDrainDispatch;
        REL::Relocation<ReturnFn*> originalDrainReturn;
        REL::Relocation<DispatchFn*> originalRetryDispatch;
        REL::Relocation<RetryPassFn*> originalRetryPass;
        REL::Relocation<AttemptFn*> originalTaskletCall;
        REL::Relocation<AttemptFn*> originalResumeCall;
        REL::Relocation<QueueFn*> originalQueueCall;
        // Set when the queue call of AttemptFunctionCall is patched (test builds only): the address
        // that call returns to, which tells its hook that AttemptFunctionCall is the caller.
        std::uintptr_t queueCallReturn{};

        Mode mode{ Mode::kCarried };

        // Every call this module patched, with the target it wrote. If another plugin later
        // rewrites one without passing through, the checks could stop seeing calls, so extra cycles
        // turn themselves off instead.
        struct Patched
        {
            std::uintptr_t call;
            std::uintptr_t target;
            const char* name;
        };
        std::vector<Patched> patched;
        // Set for the rest of the session when a patched call changed or a rule violation was seen.
        std::atomic_bool disabled{};

        // True when the previous frame ended with the gate open and the smoothed frame rate below the
        // engage rate. Only then do the hooks collect anything, and only then may this frame run
        // extra cycles.
        std::atomic_bool frameActive{};

        // Leaf lock: never held while calling into the engine. The pop hooks take it while the
        // engine holds funcQueueLock, and nothing takes funcQueueLock while holding it.
        std::mutex lock;
        // carried: scripts whose queued message was still waiting after this frame's stock update.
        // A script waits on at most one message; its entry is removed when that message runs.
        std::unordered_set<RE::VMStackID> carried;
        // accelerate: scripts that ran a state-changing native this frame, queued or direct.
        std::unordered_set<RE::VMStackID> changedThisFrame;
        // accelerate: stacks from this ID on were created during this frame. IDs only grow.
        std::atomic<RE::VMStackID> frameFirstStack{ 0 };
        // Messages an extra cycle ran although the mode's rule says they must wait. Expected zero;
        // the first one turns extra cycles off for the session.
        std::atomic<std::uint64_t> violations{};
        // Messages extra cycles dispatched (calls and returns).
        std::atomic<std::uint64_t> extraCalls{};
        // accelerate: state-changing natives called directly from tasklets while active.
        std::atomic<std::uint64_t> directChanges{};
        // Test control; see VMCatchUp::SetDirectJudgement.
        std::atomic_bool directJudgement{ true };
        // Test control; see VMCatchUp::SetExclusionList.
        std::atomic_bool exclusionList{ true };
        // Test control; see VMCatchUp::SetReadRule.
        std::atomic<VMCatchUp::ReadRule> readRule{ VMCatchUp::ReadRule::kReviewed };

        // The reviewed reads of NativeClassification.h, by implementation. Filled once by Install,
        // before any hook runs, and only read afterwards.
        struct ImageRange
        {
            std::uintptr_t begin{};
            std::uintptr_t end{};

            [[nodiscard]] bool Holds(std::uintptr_t address) const { return address >= begin && address < end; }
        };
        ImageRange gameImage;
        // Empty unless SKSE is the version whose natives were reviewed.
        ImageRange skseImage;
        // The callback address of each entry of reviewedReads; zero for SKSE entries.
        std::array<std::uintptr_t, reviewedReads.size()> reviewedCallbacks{};
        // Whether ReportRejected has logged an entry of reviewedReads; the last three are the helper
        // classes Math, StringUtil and Debug.
        std::array<std::atomic_bool, reviewedReads.size() + 3> rejectionReported{};
        // The game's native function objects and SKSE64's keep the callback at this offset: the
        // game's registration code stores it with `mov [object+0x50], rax` (ID 55394) or through a
        // constructor that does (IDs 54821, 54822, 54833), and skse64 declares m_callback there.
        constexpr std::size_t nativeCallbackOffset = 0x50;
        // Function objects of the game that report GetIsNative() but are not native function objects
        // and hold no callback: the getter and the setter of an auto property and the stub of a
        // native that is not bound (primary vtables, IDs 252795, 252797, 252808; slot 8 of each is
        // `mov al, 1; ret`). Nothing past their vtable pointer may be read.
        constexpr std::array<std::uint64_t, 3> accessorVtableIDs{ 252795, 252797, 252808 };
        std::array<std::uintptr_t, accessorVtableIDs.size()> accessorVtables{};
        // The first of them, the getter of an auto property, is a read: its Call (ID 105248) copies
        // the script variable into the stack's return value and writes nothing else of the script,
        // its type or the function object (verification/ReadList-Review.md). The VM builds a new
        // getter object for every access (ID 105232) and the setter comes from the same allocator, so
        // it is told apart by its vtable on every call, never by its address. A property with a
        // hand-written Get function is a ScriptFunction and does not come here at all.
        constexpr std::size_t propertyGetterIndex = 0;
        // Test control; see VMCatchUp::SetPropertyReads.
        std::atomic_bool propertyReads{ true };
        // Test control; see VMCatchUp::SetHoldChangeCalls.
        std::atomic_bool holdChangeCalls{ false };
        // Test builds with the queue call patched: scripts whose queued native call, not yet
        // dispatched, is judged a change (or could not be judged). Guarded by `lock`.
        std::unordered_set<RE::VMStackID> pendingChangeCall;
        std::atomic<std::uint64_t> changeCallHolds{};
        // The skip rule; see VMCatchUp::SetSkipBlocked, its only writer.
        std::atomic_bool skipBlocked{ false };
        std::atomic<std::uint64_t> frontSkips{};
        std::atomic<std::uint64_t> orderRebuilds{};
        // Test control; see VMCatchUp::SetCensus.
        std::atomic_bool census{ false };
        std::mutex censusLock;
        // Keyed by "class.name": function objects are not stable keys, because the accessors of auto
        // properties are created and released again and their addresses get reused.
        std::unordered_map<std::string, VMCatchUp::CensusEntry> censusCounts;
        // Census only, both guarded by `lock`: the native that marked each script this frame, and how
        // often each reason kept the call at the front of the queue waiting.
        std::unordered_map<RE::VMStackID, std::string> markedBy;
        std::unordered_map<std::string, std::uint64_t> blockerCounts;

        thread_local bool inExtraCycle{};
        thread_local bool blockedInCycle{};
        // Skip rule: scripts whose waiting call this extra cycle moved behind the
        // others. Reaching one of them again means the cycle has been through the whole queue.
        thread_local std::vector<RE::VMStackID> movedBehind;
        thread_local unsigned dispatchDepth{};
        // Set while AttemptFunctionCall holds a reference to the function; the function object may
        // be released as soon as that call returns, so only the classification is kept.
        enum class Prepared : std::uint8_t { kNone, kRead, kChange };
        thread_local Prepared prepared{ Prepared::kNone };
        // Set while a tasklet's call goes through AttemptFunctionCall on this thread.
        thread_local std::optional<RE::VMStackID> taskletStack;
        // Census only: "class.name" of the native that made `prepared` a change.
        thread_local std::string preparedBy;

        std::uintptr_t CallTarget(std::uintptr_t call)
        {
            std::int32_t displacement{};
            std::memcpy(&displacement, reinterpret_cast<const void*>(call + 1), sizeof(displacement));
            return call + 5 + displacement;
        }

        void Record(std::uintptr_t call, const char* name)
        {
            patched.push_back({ call, CallTarget(call), name });
        }

        // Returns false once any patched call no longer reaches this module.
        bool HooksIntact()
        {
            if (disabled.load(std::memory_order_relaxed)) {
                return false;
            }
            for (const auto& site : patched) {
                if (*reinterpret_cast<const std::uint8_t*>(site.call) != 0xE8 || CallTarget(site.call) != site.target) {
                    disabled.store(true, std::memory_order_relaxed);
                    SKSE::log::warn("VM catch-up turned off: the {} call was changed by another plugin after installation", site.name);
                    return false;
                }
            }
            return true;
        }

        bool Active()
        {
            return frameActive.load(std::memory_order_acquire);
        }

        // carried ---------------------------------------------------------------------------

        void RecordCarried()
        {
            auto* vm = VirtualMachine::GetSingleton();
            std::vector<RE::VMStackID> waiting;
            if (vm) {
                RE::BSSpinLockGuard guard(vm->funcQueueLock);
                for (auto* node = vm->funcMsgQueue.head; node; node = node->next) {
                    if (const auto* stack = reinterpret_cast<const Message*>(node->elem)->stack.get()) {
                        waiting.push_back(stack->stackID);
                    }
                }
                for (const auto& message : vm->overflowFuncMsgs) {
                    if (const auto* stack = message.stack.get()) {
                        waiting.push_back(stack->stackID);
                    }
                }
            }
            const std::lock_guard guard(lock);
            carried.clear();
            carried.insert(waiting.begin(), waiting.end());
        }

        void UpdateGame(RE::SkyrimVM* skyrimVM, float budget)
        {
            originalUpdateGame(skyrimVM, budget);
            // The stock drain of this frame is over and this frame's tasklets have not run yet.
            if (Active()) {
                RecordCarried();
            }
        }

        // accelerate ------------------------------------------------------------------------

        ImageRange ImageOf(std::uintptr_t base)
        {
            if (base == 0) {
                return {};
            }
            // PE headers: e_lfanew at +0x3C; SizeOfImage at +0x50 of the NT headers.
            const auto headers = base + *reinterpret_cast<const std::int32_t*>(base + 0x3C);
            return { base, base + *reinterpret_cast<const std::uint32_t*>(headers + 0x50) };
        }

        void PrepareReviewedReads()
        {
            gameImage = ImageOf(REL::Module::get().base());
            std::size_t gameEntries = 0;
            for (std::size_t i = 0; i < reviewedReads.size(); ++i) {
                const bool game = reviewedReads[i].origin == NativeOrigin::kGame;
                reviewedCallbacks[i] = game ? REL::ID(reviewedReads[i].callbackID).address() : 0;
                gameEntries += game ? 1 : 0;
            }
            for (std::size_t i = 0; i < accessorVtableIDs.size(); ++i) {
                accessorVtables[i] = REL::ID(accessorVtableIDs[i]).address();
            }
            const auto skseVersion = SKSE::GetSKSEVersion();
            skseImage = skseVersion == reviewedSKSEVersion ?
                ImageOf(reinterpret_cast<std::uintptr_t>(REX::W32::GetModuleHandleW(L"skse64_1_6_1170.dll"))) : ImageRange{};
            if (skseImage.end != 0) {
                SKSE::log::info("VM catch-up reads: {} game natives and {} SKSE natives count as reads", gameEntries,
                    reviewedReads.size() - gameEntries);
            } else {
                SKSE::log::info("VM catch-up reads: {} game natives count as reads; SKSE natives (StringUtil included) do not, because SKSE {:08X} is not the reviewed {:08X}",
                    gameEntries, skseVersion, reviewedSKSEVersion);
            }
        }

        bool IsAccessor(std::uintptr_t vtable)
        {
            return std::ranges::find(accessorVtables, vtable) != accessorVtables.end();
        }

        // Math, StringUtil or Debug; `className` passed IsHelperNative.
        std::size_t HelperClass(std::string_view className)
        {
            const char first = NativeClassificationDetail::Lower(className.front());
            return first == 'm' ? 0 : first == 's' ? 1 : 2;
        }

        // A native with a listed name whose implementation is not the reviewed one counts as a
        // change, and every script that calls it waits for the next frame. That costs catch-up
        // effect without any other sign outside a test build's census, so the log says it: once per
        // reviewed native, and once per helper class with the first function it happened to.
        void ReportRejected(std::size_t slot, std::string_view className, std::string_view name, const char* reason)
        {
            auto& reported = rejectionReported[slot];
            if (reported.load(std::memory_order_relaxed) || reported.exchange(true, std::memory_order_relaxed)) {
                return;
            }
            SKSE::log::warn("VM catch-up reads: {}.{} counts as a change, because {}{}", className, name, reason,
                slot < reviewedReads.size() ? "" : " (further functions of this class are not reported)");
        }

        // Whether this native is one of the reviewed reads: the implementation that was read, not
        // merely a native of that name. `function` reports GetIsNative().
        bool IsReviewedRead(const Function* function, std::string_view className, std::string_view name)
        {
            // Only the vtable pointer, which every function object has, is read at this point.
            const auto* object = reinterpret_cast<const std::byte*>(function);
            const auto vtable = *reinterpret_cast<const std::uintptr_t*>(object);
            if (vtable == accessorVtables[propertyGetterIndex]) {
                return propertyReads.load(std::memory_order_relaxed);
            }
            if (IsAccessor(vtable)) {
                return false;  // the setter of an auto property, or the stub of an unbound native
            }
            const bool helper = IsHelperNative(className, name);
            const auto* entry = helper ? nullptr : FindReviewedRead(className, name);
            if (!helper && !entry) {
                return false;  // most natives end here; nothing past the vtable pointer is read
            }
            // From here on the name is a listed one, so a "no" is reported (ReportRejected).
            const auto slot = entry ? static_cast<std::size_t>(entry - reviewedReads.data()) :
                reviewedReads.size() + HelperClass(className);
            const bool game = gameImage.Holds(vtable);
            const bool skse = !game && skseImage.Holds(vtable);
            if (!game && !skse) {
                // Built by another plugin: its implementation was not read, whatever it is called.
                // With SKSE not the reviewed version its own natives end here as well; Install has
                // reported that, and the two cannot be told apart.
                if (skseImage.end != 0) {
                    ReportRejected(slot, className, name, "another plugin registered it");
                }
                return false;
            }
            // A function object of the game (accessors excluded above) or of SKSE that carries the
            // name of a native is a native function object, 0x58 bytes with the callback at +0x50.
            const auto callback = *reinterpret_cast<const std::uintptr_t*>(object + nativeCallbackOffset);
            bool read{};
            const char* reason{};
            if (helper) {
                // Math and Debug natives are the game's; StringUtil and the bit operations of Math
                // are SKSE's (PapyrusStringUtil.cpp, PapyrusMath.cpp). The callback must lie in the
                // image that built the object: one that was pointed elsewhere was not read.
                read = game ? gameImage.Holds(callback) : skseImage.Holds(callback);
                reason = "its callback lies outside the module that registered it";
            } else if (entry->origin == NativeOrigin::kGame) {
                read = game && callback == reviewedCallbacks[slot];
                reason = game ? "its callback is not the reviewed one" : "SKSE registered it and the game's native was the one reviewed";
            } else {
                read = skse && skseImage.Holds(callback);
                reason = skse ? "its callback lies outside SKSE" : "the game registered it and SKSE's native was the one reviewed";
            }
            if (!read) {
                ReportRejected(slot, className, name, reason);
            }
            return read;
        }

        bool ChangesState(const Function* function)
        {
            if (!function->GetIsNative()) {
                return false;  // a script function runs bytecode; its own native calls are judged separately
            }
            const auto& name = function->GetName();
            const auto& className = function->GetObjectTypeName();
            if (name.empty()) {
                return true;
            }
            const std::string_view classView = className.empty() ? "" : className.c_str();
            if (readRule.load(std::memory_order_relaxed) == VMCatchUp::ReadRule::kReviewed) {
                return !IsReviewedRead(function, classView, name.c_str());
            }
            return !IsSideEffectFreeNative(classView, name.c_str(), exclusionList.load(std::memory_order_relaxed));
        }

        std::string FunctionKey(const Function* function)
        {
            const auto& name = function->GetName();
            const auto& className = function->GetObjectTypeName();
            const std::string_view classView = className.empty() ? "" : className.c_str();
            const std::string_view nameView = name.empty() ? "" : name.c_str();
            std::string key;
            key.reserve(classView.size() + nameView.size() + 1);
            key.append(classView).append(1, '.').append(nameView);
            return key;
        }

        // Test control: `function` is a native that is about to run on this thread.
        void CountCensus(const Function* function)
        {
            const auto& name = function->GetName();
            const auto& className = function->GetObjectTypeName();
            const std::string_view classView = className.empty() ? "" : className.c_str();
            const std::string_view nameView = name.empty() ? "" : name.c_str();
            // The release rule's verdict on this very object, taken for every call: the same name can
            // come with another implementation later on.
            const bool read = IsReviewedRead(function, classView, nameView);
            const std::lock_guard guard(censusLock);
            auto [entry, added] = censusCounts.try_emplace(FunctionKey(function));
            if (added) {
                entry->second.className = classView;
                entry->second.name = nameView;
                entry->second.accessor = IsAccessor(*reinterpret_cast<const std::uintptr_t*>(function));
            }
            auto& counts = entry->second;
            ++(read ? counts.readVerdicts : counts.changeVerdicts);
            ++(taskletStack ? counts.direct : dispatchDepth == 0 ? counts.other : inExtraCycle ? counts.extra : counts.queued);
        }

        bool NewThisFrame(RE::VMStackID id)
        {
            // Unsigned distance keeps the comparison correct if the 32-bit counter ever wraps.
            return id - frameFirstStack.load(std::memory_order_acquire) < 0x80000000u;
        }

        // `by` (census only) names the native; the first one of the frame is kept.
        void MarkChanged(RE::VMStackID id, const std::string* by = nullptr)
        {
            const std::lock_guard guard(lock);
            changedThisFrame.insert(id);
            if (by) {
                markedBy.try_emplace(id, *by);
            }
        }

        std::uint64_t Prepare(FunctionPtr* function, void* a, void* b, std::uint64_t c)
        {
            if (function && function->get()) {
                const bool counting = census.load(std::memory_order_relaxed);
                if (counting && function->get()->GetIsNative()) {
                    CountCensus(function->get());
                }
                if (dispatchDepth > 0) {
                    // A queued call the drain or the retry pass is running on this thread.
                    if (prepared != Prepared::kChange) {
                        prepared = ChangesState(function->get()) ? Prepared::kChange : Prepared::kRead;
                        if (counting && prepared == Prepared::kChange) {
                            preparedBy = FunctionKey(function->get());
                        }
                    }
                } else if (taskletStack && Active() && directJudgement.load(std::memory_order_relaxed) &&
                           ChangesState(function->get())) {
                    // A tasklet runs this native now; its script's next queued call is kept out of this
                    // frame's extra cycles.
                    directChanges.fetch_add(1, std::memory_order_relaxed);
                    if (counting) {
                        const auto by = FunctionKey(function->get());
                        MarkChanged(*taskletStack, &by);
                    } else {
                        MarkChanged(*taskletStack);
                    }
                }
            }
            return originalPrepare(function, a, b, c);
        }

        std::uint64_t AttemptFromTasklet(AttemptFn* original, VirtualMachine* vm, Stack* stack, void* tasklet, bool fromTasklet)
        {
            const auto outer = taskletStack;
            taskletStack = stack ? std::optional<RE::VMStackID>(stack->stackID) : std::nullopt;
            const auto result = original(vm, stack, tasklet, fromTasklet);
            taskletStack = outer;
            return result;
        }

        std::uint64_t TaskletCall(VirtualMachine* vm, Stack* stack, void* tasklet, bool fromTasklet)
        {
            return AttemptFromTasklet(originalTaskletCall.get(), vm, stack, tasklet, fromTasklet);
        }

        std::uint64_t ResumeCall(VirtualMachine* vm, Stack* stack, void* tasklet, bool fromTasklet)
        {
            return AttemptFromTasklet(originalResumeCall.get(), vm, stack, tasklet, fromTasklet);
        }

        // Test builds only: replaces the call that queues a tasklet's native call. Records whether
        // that call is a change, for SetHoldChangeCalls.
        std::uint64_t QueueFromTasklet(VirtualMachine* vm, Stack* stack, void* query)
        {
            if (stack) {
                // The resolved function is read from AttemptFunctionCall's frame, which is only where
                // this code expects it when AttemptFunctionCall itself is the caller. If another
                // plugin's hook calls this one, the call counts as one that could not be judged.
                const auto* returnSlot = static_cast<const std::byte*>(_AddressOfReturnAddress());
                const Function* function = nullptr;
                if (*reinterpret_cast<const std::uintptr_t*>(returnSlot) == queueCallReturn) {
                    function = *reinterpret_cast<const Function* const*>(returnSlot + sizeof(std::uintptr_t) + resolvedFunctionOffset);
                }
                const bool change = !function || ChangesState(function);
                const std::lock_guard guard(lock);
                if (change) {
                    pendingChangeCall.insert(stack->stackID);
                } else {
                    pendingChangeCall.erase(stack->stackID);
                }
            }
            return originalQueueCall(vm, stack, query);
        }

        // both ------------------------------------------------------------------------------

        // Caller holds lock. Test control: this script's queued call is itself a change.
        bool HeldCallLocked(RE::VMStackID id)
        {
            return holdChangeCalls.load(std::memory_order_relaxed) && pendingChangeCall.contains(id);
        }

        // Caller holds lock. Whether the message of this stack must wait for the next stock update.
        bool MustWaitLocked(const Stack* stack)
        {
            if (!stack) {
                return true;
            }
            if (mode == Mode::kCarried) {
                return !carried.contains(stack->stackID);
            }
            return NewThisFrame(stack->stackID) || changedThisFrame.contains(stack->stackID) || HeldCallLocked(stack->stackID);
        }

        // Called for every message an extra cycle runs.
        void CheckExtraCycleMessage(const Stack* stack)
        {
            if (!inExtraCycle || !stack) {
                return;
            }
            extraCalls.fetch_add(1, std::memory_order_relaxed);
            bool violated = false;
            {
                const std::lock_guard guard(lock);
                violated = MustWaitLocked(stack);
                if (mode == Mode::kCarried) {
                    carried.erase(stack->stackID);  // its next call waits for the next frame
                }
            }
            if (!violated) {
                return;
            }
            violations.fetch_add(1, std::memory_order_relaxed);
            // The message the engine took is not the front that was judged, so the queue premise no
            // longer holds (for example another plugin working on the queue). This call cannot be
            // taken back; the pop hooks stop the cycle and no later frame starts one.
            if (!disabled.exchange(true, std::memory_order_relaxed)) {
                SKSE::log::warn("VM catch-up turned off: an extra cycle ran a call of stack {} that the mode's rule holds for the next frame",
                    stack->stackID);
            }
        }

        std::uint64_t DispatchChecked(DispatchFn* original, VirtualMachine* vm, Stack* stack, const QueryPtr* query, bool flag)
        {
            CheckExtraCycleMessage(stack);
            if (queueCallReturn != 0 && stack) {
                // Test builds: this script's queued call leaves the queue now. If it cannot run yet it
                // moves to the suspended stacks, which only stock updates retry.
                const std::lock_guard guard(lock);
                pendingChangeCall.erase(stack->stackID);
            }
            if (mode != Mode::kAccelerate || !Active()) {
                return original(vm, stack, query, flag);
            }
            // Taken before the call: the stack's last reference may be released while it runs.
            const auto id = stack ? std::optional<RE::VMStackID>(stack->stackID) : std::nullopt;
            const auto outer = prepared;
            prepared = Prepared::kNone;
            ++dispatchDepth;
            const auto result = original(vm, stack, query, flag);
            --dispatchDepth;
            const auto observed = prepared;
            prepared = outer;
            // kNone means the call was re-queued before its function was prepared, so it did not run.
            if (id && observed == Prepared::kChange) {
                MarkChanged(*id, census.load(std::memory_order_relaxed) ? &preparedBy : nullptr);
            }
            return result;
        }

        std::uint64_t DrainDispatch(VirtualMachine* vm, Stack* stack, const QueryPtr* query, bool flag)
        {
            return DispatchChecked(originalDrainDispatch.get(), vm, stack, query, flag);
        }

        std::uint64_t RetryDispatch(VirtualMachine* vm, Stack* stack, const QueryPtr* query, bool flag)
        {
            return DispatchChecked(originalRetryDispatch.get(), vm, stack, query, flag);
        }

        std::uint64_t DrainReturn(VirtualMachine* vm, Stack* stack, std::uint64_t a, std::uint64_t b)
        {
            CheckExtraCycleMessage(stack);
            return originalDrainReturn(vm, stack, a, b);
        }

        std::uint32_t ReadCount(const void* object, std::size_t offset)
        {
            return object ? *reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(object) + offset) : 0;
        }

        void RetryPass(VirtualMachine* vm, float budget, std::uint32_t* waiting)
        {
            if (!inExtraCycle) {
                originalRetryPass(vm, budget, waiting);
                return;
            }
            // Report the same total the stock pass reports, without resuming anything.
            RE::BSSpinLockGuard guard(vm->suspendQueueLock);
            *waiting = ReadCount(vm->stacksToSuspend, queueEntriesOffset) + ReadCount(vm->stacksToSuspendOverflow, arraySizeOffset) +
                ReadCount(vm->stacksToResume, queueEntriesOffset) + ReadCount(vm->stacksToResumeOverflow, arraySizeOffset);
        }

        // The call the next pop takes, judged by the mode's rule.
        enum class Front : std::uint8_t
        {
            kEmpty,     // nothing is queued
            kMustWait,  // the front call waits for the next stock update, and so does everything behind it
            kMayRun,
        };

        // Caller holds lock. `stack` is the script of a call that was found to wait: counts why, for
        // the test controls. One count per check that ended so (per call moved, with the skip rule).
        void CountBlockedLocked(const Stack* stack)
        {
            if (mode != Mode::kAccelerate) {
                return;
            }
            if (stack && !NewThisFrame(stack->stackID) && !changedThisFrame.contains(stack->stackID)) {
                // Held by the test control alone: the call itself is a change.
                changeCallHolds.fetch_add(1, std::memory_order_relaxed);
            }
            if (!census.load(std::memory_order_relaxed)) {
                return;
            }
            std::string reason;
            if (!stack) {
                reason = "(message without a stack)";
            } else if (NewThisFrame(stack->stackID)) {
                reason = "(script started this frame)";
            } else if (const auto by = markedBy.find(stack->stackID); by != markedBy.end()) {
                reason = by->second;
            } else if (changedThisFrame.contains(stack->stackID)) {
                reason = "(change, native not recorded)";
            } else {
                reason = "(queued change call held)";
            }
            ++blockerCounts[reason];
        }

        // The message the stock pop (ID 104879) takes next: the head of the linked queue, else the
        // last overflow entry. Caller holds funcQueueLock, as the drain does while it pops. Under that
        // lock the list pop cannot fail while the list has entries, and the stock pop takes the newest
        // overflow entry only when the list is empty.
        const Message* PeekFront(VirtualMachine* vm)
        {
            if (vm->funcMsgQueue.head) {
                return reinterpret_cast<const Message*>(vm->funcMsgQueue.head->elem);
            }
            const auto count = vm->overflowFuncMsgs.size();
            return count != 0 ? &vm->overflowFuncMsgs[count - 1] : nullptr;
        }

        // Caller holds funcQueueLock.
        Front FrontState(VirtualMachine* vm)
        {
            const auto* front = PeekFront(vm);
            if (!front) {
                return Front::kEmpty;
            }
            const std::lock_guard guard(lock);
            const auto* stack = front->stack.get();
            if (!MustWaitLocked(stack)) {
                return Front::kMayRun;
            }
            CountBlockedLocked(stack);
            return Front::kMustWait;
        }

        void TurnOffForQueue(const char* what)
        {
            if (!disabled.exchange(true, std::memory_order_relaxed)) {
                SKSE::log::warn("VM catch-up turned off: {}", what);
            }
        }

        // Puts a message the stock pop handed out back at the end of the queue, the way the stock
        // push (ID 104848 from +0xB5) does it: the linked queue first, the overflow array when that
        // has no free node, then the waiting count the pop decremented. The profiler callback that
        // ID 104848 makes first is left out on purpose: the call was announced when it was queued.
        // Returns whether the message went to the linked queue. Caller holds funcQueueLock;
        // `message` keeps its references.
        bool PushBack(VirtualMachine* vm, Message& message)
        {
            const bool linked = vm->funcMsgQueue.Push(&message);
            if (!linked) {
                vm->overflowFuncMsgs.push_back(message);
            }
            ++vm->uiWaitingFunctionMessages;
            return linked;
        }

        bool WasMovedBehind(RE::VMStackID id)
        {
            return std::ranges::find(movedBehind, id) != movedBehind.end();
        }

        // Skip rule. Takes the front message, which must wait, and queues it again
        // behind the others. Returns false when the queue did not behave as this code takes it to; no
        // further call is moved then. Caller holds funcQueueLock.
        bool MoveFrontBehind(PopFn* original, VirtualMachine* vm, Message* scratch, RE::VMStackID id)
        {
            if (!original(vm, scratch)) {
                return false;
            }
            const auto* taken = scratch->stack.get();
            const bool expected = taken && taken->stackID == id;
            // The pop has just given a node of the linked queue back, or the linked queue is empty,
            // so the push finds a free node. The overflow array would hand the message out in the
            // wrong place: the stock pop takes that array from its end.
            const bool linked = PushBack(vm, *scratch);
            scratch->stack.reset();
            scratch->funcQuery.reset();
            if (!expected) {
                // The pop handed out another message than the one that was looked at (for example
                // another plugin working on the queue). The message is queued again, but its place
                // cannot be restored.
                TurnOffForQueue("the queue front changed between the look and the pop");
                return false;
            }
            if (!linked) {
                TurnOffForQueue("a call taken from the linked queue could not be put back into it");
                return false;
            }
            movedBehind.push_back(id);
            frontSkips.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Skip rule: instead of ending the extra cycle at a call that must wait,
        // move that call behind the others and go on. A call is moved once per cycle, and the cycle
        // is over for the queue when the front is a call it already moved or a message without a
        // script. RestoreQueueOrder puts the queue in order again afterwards.
        //
        // A moved call goes to the end of the linked queue, in front of the overflow array. A cycle
        // therefore reaches the calls of the linked queue only, at most the 1,024 its node pool holds
        // (VirtualMachine::funcMsgPool), which also bounds the pops and pushes of one cycle; calls in
        // the overflow array run when the stock drain gets to them. AnyState looks at the same calls.
        bool PopSkippingBlocked(PopFn* original, VirtualMachine* vm, Message* out)
        {
            for (;;) {
                const auto* front = PeekFront(vm);
                if (!front) {
                    return false;
                }
                const auto* stack = front->stack.get();
                if (!stack || WasMovedBehind(stack->stackID)) {
                    return false;  // a message that cannot be identified, or once around the queue
                }
                bool wait = false;
                {
                    const std::lock_guard guard(lock);
                    wait = MustWaitLocked(stack);
                    if (wait) {
                        CountBlockedLocked(stack);
                    }
                }
                if (!wait) {
                    return original(vm, out);
                }
                if (!MoveFrontBehind(original, vm, out, stack->stackID)) {
                    return false;
                }
            }
        }

        // Takes every call out of the linked queue and puts them back in the order of
        // QueueOrder::Restored. As many nodes are free afterwards as were taken, so every call fits
        // again; the overflow array stays as it is, behind the linked queue. Caller holds
        // funcQueueLock, so no other thread sees the queue without these calls: every gameplay push
        // and the save path take that lock.
        void RebuildLinkedQueue(VirtualMachine* vm)
        {
            constexpr std::uint64_t noStack = ~std::uint64_t{ 0 };
            std::vector<Message> taken;
            std::vector<std::uint64_t> keys;
            Message scratch{};
            while (vm->funcMsgQueue.head && originalFirstPop(vm, &scratch)) {
                const auto* stack = scratch.stack.get();
                keys.push_back(stack ? std::uint64_t{ stack->stackID } : noStack);
                taken.push_back(scratch);
            }
            scratch.stack.reset();
            scratch.funcQuery.reset();
            const std::vector<std::uint64_t> moved(movedBehind.begin(), movedBehind.end());
            bool linked = true;
            for (const auto index : QueueOrder::Restored<std::uint64_t>(keys, moved)) {
                linked = PushBack(vm, taken[index]) && linked;
            }
            if (!linked) {
                TurnOffForQueue("the linked queue did not take back the calls that were taken from it");
            }
        }

        // After an extra cycle's update (skip rule). The cycle leaves the calls it
        // moved at the end of the linked queue. Usually they are its whole content, in their order.
        // Otherwise calls the drain never reached (it ran out of budget) stand in front of them, or
        // calls that were queued while the cycle ran (by a native it executed, or by another thread)
        // stand between them; the linked queue is then rebuilt, so that the calls that had to wait
        // keep the places they had among themselves and before everything that came later. Only the
        // calls the cycle ran have overtaken them. Takes funcQueueLock.
        void RestoreQueueOrder(VirtualMachine* vm)
        {
            if (movedBehind.empty()) {
                return;
            }
            RE::BSSpinLockGuard guard(vm->funcQueueLock);
            bool inFront = true;
            const auto* node = vm->funcMsgQueue.head;
            for (const auto id : movedBehind) {
                const auto* stack = node ? reinterpret_cast<const Message*>(node->elem)->stack.get() : nullptr;
                if (!stack || stack->stackID != id) {
                    inFront = false;
                    break;
                }
                node = node->next;
            }
            if (!inFront) {
                RebuildLinkedQueue(vm);
                orderRebuilds.fetch_add(1, std::memory_order_relaxed);
            }
            movedBehind.clear();
        }

        bool PopChecked(PopFn* original, VirtualMachine* vm, Message* out)
        {
            if (!inExtraCycle) {
                return original(vm, out);
            }
            if (disabled.load(std::memory_order_relaxed)) {
                blockedInCycle = true;
                return false;
            }
            if (mode == Mode::kAccelerate && skipBlocked.load(std::memory_order_relaxed)) {
                return PopSkippingBlocked(original, vm, out);
            }
            if (FrontState(vm) == Front::kMustWait) {
                blockedInCycle = true;
                return false;
            }
            return original(vm, out);
        }

        // Skip rule: whether a call may run that an extra cycle can get to, which
        // is what PopSkippingBlocked reaches: the calls of the linked queue up to a message without a
        // script, or, when the linked queue is empty, only the overflow entry the next pop takes
        // (moved, it becomes the linked queue's only call and the cycle ends at it).
        // Caller holds funcQueueLock.
        Front AnyState(VirtualMachine* vm)
        {
            const std::lock_guard guard(lock);
            if (!vm->funcMsgQueue.head) {
                const auto count = vm->overflowFuncMsgs.size();
                if (count == 0) {
                    return Front::kEmpty;
                }
                return MustWaitLocked(vm->overflowFuncMsgs[count - 1].stack.get()) ? Front::kMustWait : Front::kMayRun;
            }
            for (const auto* node = vm->funcMsgQueue.head; node; node = node->next) {
                const auto* stack = reinterpret_cast<const Message*>(node->elem)->stack.get();
                if (!stack) {
                    break;
                }
                if (!MustWaitLocked(stack)) {
                    return Front::kMayRun;
                }
            }
            return Front::kMustWait;
        }

        Front PendingState(VirtualMachine* vm)
        {
            return mode == Mode::kAccelerate && skipBlocked.load(std::memory_order_relaxed) ? AnyState(vm) : FrontState(vm);
        }

        bool FirstPop(VirtualMachine* vm, Message* out) { return PopChecked(originalFirstPop.get(), vm, out); }
        bool NextPop(VirtualMachine* vm, Message* out) { return PopChecked(originalNextPop.get(), vm, out); }

        // Called when the gameplay VM job ends. The next stock update starts the next frame, and the
        // hooks collect during it only when `collect` is set.
        void EndFrame(bool collect)
        {
            RE::VMStackID next = 0;
            if (collect && mode == Mode::kAccelerate) {
                if (auto* vm = VirtualMachine::GetSingleton()) {
                    RE::BSSpinLockGuard guard(vm->runningStacksLock);
                    next = vm->nextStackID;
                }
            }
            {
                const std::lock_guard guard(lock);
                carried.clear();
                changedThisFrame.clear();
                markedBy.clear();
                frameFirstStack.store(next, std::memory_order_release);
            }
            frameActive.store(collect, std::memory_order_release);
        }

        void Reset()
        {
            const std::lock_guard guard(lock);
            carried.clear();
            changedThisFrame.clear();
            markedBy.clear();
            pendingChangeCall.clear();
        }

        struct Addresses
        {
            std::uintptr_t job;
            std::uintptr_t update;
            std::uintptr_t attempt;
            std::uintptr_t drain;
            std::uintptr_t retry;
            std::uintptr_t tasklet;
            std::uintptr_t resume;
        };

        template <std::size_t N>
        bool Matches(std::uintptr_t function, const Site& site, const std::array<std::uint8_t, N>& code,
            const char* name, std::vector<const char*>& chained)
        {
            if (code[site.call] != 0xE8) {
                return false;
            }
            bool hooked = false;
            if (!MatchesCode(function + site.offset, code, std::array{ site.call }, hooked)) {
                SKSE::log::warn("VM catch-up: {} code differs from AE 1.6.1170", name);
                return false;
            }
            if (hooked) {
                chained.push_back(name);
            }
            return true;
        }

        bool Verify(Mode wanted, bool observeQueuedCalls, const Addresses& at, std::vector<const char*>& chained)
        {
            if (wanted == Mode::kAccelerate && observeQueuedCalls &&
                !Matches(at.attempt, queueCall, queueCallCode, "tasklet queue call", chained)) {
                return false;
            }
            const bool common = Matches(at.drain, firstPop, firstPopCode, "first queue pop", chained) &&
                Matches(at.drain, nextPop, nextPopCode, "next queue pop", chained) &&
                Matches(at.drain, drainDispatch, drainDispatchCode, "queue dispatch", chained) &&
                Matches(at.drain, drainReturn, drainReturnCode, "queue return dispatch", chained) &&
                Matches(at.update, retryPass, retryPassCode, "retry pass", chained);
            if (!common) {
                return false;
            }
            if (wanted == Mode::kCarried) {
                return Matches(at.job, updateGame, updateGameCode, "gameplay VM update", chained);
            }
            return Matches(at.attempt, prepare, prepareCode, "argument setup", chained) &&
                Matches(at.retry, retryDispatch, retryDispatchCode, "suspended retry", chained) &&
                Matches(at.tasklet, taskletCall, taskletCallCode, "tasklet call", chained) &&
                Matches(at.resume, resumeCall, resumeCallCode, "tasklet resume call", chained);
        }

        void Install(Mode wanted, bool observeQueuedCalls, const Addresses& at)
        {
            mode = wanted;
            auto& trampoline = SKSE::GetTrampoline();
            const auto call = [](std::uintptr_t function, const Site& site) { return function + site.offset + site.call; };
            if (wanted == Mode::kAccelerate) {
                PrepareReviewedReads();
            }
            if (wanted == Mode::kAccelerate && observeQueuedCalls) {
                const auto site = call(at.attempt, queueCall);
                originalQueueCall = trampoline.write_call<5>(site, QueueFromTasklet);
                Record(site, "tasklet queue call");
                queueCallReturn = site + 5;
            }
            if (wanted == Mode::kCarried) {
                originalUpdateGame = trampoline.write_call<5>(call(at.job, updateGame), UpdateGame);
                Record(call(at.job, updateGame), "gameplay VM update");
            } else {
                originalPrepare = trampoline.write_call<5>(call(at.attempt, prepare), Prepare);
                Record(call(at.attempt, prepare), "argument setup");
                originalRetryDispatch = trampoline.write_call<5>(call(at.retry, retryDispatch), RetryDispatch);
                Record(call(at.retry, retryDispatch), "suspended retry");
                originalTaskletCall = trampoline.write_call<5>(call(at.tasklet, taskletCall), TaskletCall);
                Record(call(at.tasklet, taskletCall), "tasklet call");
                originalResumeCall = trampoline.write_call<5>(call(at.resume, resumeCall), ResumeCall);
                Record(call(at.resume, resumeCall), "tasklet resume call");
            }
            originalFirstPop = trampoline.write_call<5>(call(at.drain, firstPop), FirstPop);
            Record(call(at.drain, firstPop), "first queue pop");
            originalNextPop = trampoline.write_call<5>(call(at.drain, nextPop), NextPop);
            Record(call(at.drain, nextPop), "next queue pop");
            originalDrainDispatch = trampoline.write_call<5>(call(at.drain, drainDispatch), DrainDispatch);
            Record(call(at.drain, drainDispatch), "queue dispatch");
            originalDrainReturn = trampoline.write_call<5>(call(at.drain, drainReturn), DrainReturn);
            Record(call(at.drain, drainReturn), "queue return dispatch");
            originalRetryPass = trampoline.write_call<5>(call(at.update, retryPass), RetryPass);
            Record(call(at.update, retryPass), "retry pass");
        }
    }

    // What an extra cycle would start from.
    struct Pending
    {
        Chain::Front front;
        // Tasklets already waiting for the next TasksToJobs. Before an extra cycle these were not
        // resumed by that cycle's calls: they reached the engine's op limit in the previous batch,
        // or a latent call returned to them.
        std::uint32_t tasklets;
    };

    Pending ReadPending(VirtualMachine* vm)
    {
        // TasksToJobs, the native-call drain and every gameplay push take this lock.
        RE::BSSpinLockGuard guard(vm->funcQueueLock);
        return { Chain::PendingState(vm), vm->vmTasks.size() };
    }

    // What the extra cycles of the current frame ran, kept until the frame ends so that a frame
    // whose extra work overran can say which scripts it was (MaybeReportSlowFrame). Only a reference
    // to each function object is taken while the frame runs; names are read only for a report. Both
    // vectors get their capacity once at Install and never grow, so nothing is allocated per frame.
    // Guarded by stateLock.
    using FunctionRef = RE::BSTSmartPointer<RE::BSScript::IFunction>;
    struct CycleRecord
    {
        double updateMS;
        double batchMS;
        std::uint32_t tasklets;    // waiting when the batch started
        std::size_t firstFunction;  // into frameFunctions
        std::size_t functions;      // recorded; fewer than `tasklets` once the capacity is used up
    };
    constexpr std::size_t maxRecordedFunctions = 256;
    std::vector<CycleRecord> frameCycles;
    std::vector<FunctionRef> frameFunctions;

    // The tasklets waiting for the next TasksToJobs, and for each the function its stack runs next
    // (the top frame). They have not started, so their stacks do not change under funcQueueLock.
    std::uint32_t TakeWaitingTasklets(VirtualMachine* vm)
    {
        RE::BSSpinLockGuard guard(vm->funcQueueLock);
        for (const auto* tasklet : vm->vmTasks) {
            if (frameFunctions.size() == frameFunctions.capacity()) {
                break;
            }
            const auto* stack = tasklet ? tasklet->stack : nullptr;
            const auto* frame = stack ? stack->top : nullptr;
            frameFunctions.push_back(frame ? frame->owningFunction : FunctionRef{});
        }
        return vm->vmTasks.size();
    }

    // One line per extra cycle of a frame that overran: how long its Update and its tasklet batch
    // took, and which script functions the batch ran. The slow part has been the batch (a script's
    // own bytecode or a native it called directly), which no counter can attribute.
    void ReportSlowFrame()
    {
        try {
            for (std::size_t i = 0; i < frameCycles.size(); ++i) {
                const auto& cycle = frameCycles[i];
                std::vector<std::string> names;
                names.reserve(cycle.functions);
                for (std::size_t f = cycle.firstFunction; f < cycle.firstFunction + cycle.functions; ++f) {
                    const auto& function = frameFunctions[f];
                    if (!function) {
                        names.emplace_back();
                        continue;
                    }
                    const auto& type = function->GetObjectTypeName();
                    const auto& name = function->GetName();
                    names.push_back(std::format("{}.{}", type.empty() ? "?" : type.c_str(), name.empty() ? "?" : name.c_str()));
                }
                SKSE::log::info("VM catch-up slow frame: cycle {} of {}: update {:.3f} ms, {} tasklets in {:.3f} ms: {}{}", i + 1,
                    frameCycles.size(), cycle.updateMS, cycle.tasklets, cycle.batchMS, SlowFrameReport::Summarize(names, 12),
                    cycle.functions < cycle.tasklets ? std::format(" (only {} recorded)", cycle.functions) : std::string{});
            }
        } catch (...) {
            // A report line is not worth anything that could go wrong in the game's VM job.
        }
    }

    bool Frozen(RE::SkyrimVM* skyrimVM, VirtualMachine* vm)
    {
        auto& data = skyrimVM->GetVMRuntimeData();
        {
            RE::BSSpinLockGuard guard(data.frozenLock);
            if (data.isFrozen != 0) {
                return true;
            }
        }
        RE::BSSpinLockGuard guard(vm->frozenStacksLock);
        return vm->freezeState.underlying() != 0;
    }

    float PassBudget()
    {
        const float value = *updateBudgetMS;
        if (!std::isfinite(value) || value <= 0.0f) {
            return minimumUpdateMS;
        }
        return std::clamp(value * stockUpdateShare, minimumUpdateMS, maximumUpdateMS);
    }

    void MaybeLog(Clock::time_point now)
    {
        if (settings.logIntervalSeconds == 0 || now < nextLog) {
            return;
        }
        nextLog = now + std::chrono::seconds(settings.logIntervalSeconds);
        if (stats.frames == 0) {
            return;
        }
        SKSE::log::info(
            "VM catch-up: frames={} slow={} engaged={} avg_frame_ms={:.2f} extra_cycles={} with_tasklets={} extra_calls={} leftover_tasklets={} idle_stops={} "
            "time_stops={} blocked_stops={} blocked_skips={} front_skips={} order_rebuilds={} guard_violations={} direct_changes={} frozen_stops={} held_frames={} overrun_pauses={} gate_closes={} extra_ms_per_frame={:.4f} max_frame_extra_ms={:.3f} max_update_ms={:.3f} "
            "max_batch_ms={:.3f} slowest_batch_tasklets={} max_batch_tasklets={}",
            stats.frames, stats.slowFrames, stats.engagedFrames, stats.intervalMS / static_cast<double>(stats.frames), stats.cycles,
            stats.taskletCycles, Chain::extraCalls.exchange(0, std::memory_order_relaxed), stats.leftoverTasklets, stats.idleStops,
            stats.timeStops, stats.blockedStops, stats.blockedSkips,
            Chain::frontSkips.exchange(0, std::memory_order_relaxed), Chain::orderRebuilds.exchange(0, std::memory_order_relaxed),
            Chain::violations.exchange(0, std::memory_order_relaxed), Chain::directChanges.exchange(0, std::memory_order_relaxed),
            stats.frozenStops, stats.heldFrames,
            stats.overrunPauses, stats.gateCloses, stats.extraMS / static_cast<double>(stats.frames), stats.maxFrameExtraMS, stats.maxUpdateMS,
            stats.maxBatchMS, stats.slowestBatchTasklets, stats.maxBatchTasklets);
        stats = {};
    }

    // The periodic report is written from frames with the gate open. A gate that stayed closed would
    // leave no line at all, and extra cycles that never run would look the same as a quiet log. So
    // one line per closed stretch says it, once the stretch has lasted a report interval. A pause, a
    // menu or a loading screen that long is normal; such a line while the game is being played is
    // not. The count of VM updates tells a stretch the game spent loading (few) from one in which
    // the VM kept running behind a closed gate.
    void ReportClosedGate(Clock::time_point now)
    {
        if (settings.logIntervalSeconds == 0) {
            return;
        }
        if (closedSince == Clock::time_point{}) {
            closedSince = now;
            closedUpdates = 0;
            closedReported = false;
        }
        ++closedUpdates;
        if (closedReported || now - closedSince < std::chrono::seconds(settings.logIntervalSeconds)) {
            return;
        }
        closedReported = true;
        SKSE::log::info("VM catch-up: the gameplay gate has been closed for {:.0f} s and {} VM updates (no save loaded yet, game "
                        "paused, or the loading or main menu open); no extra cycles run until it opens. gate_closes={}",
            std::chrono::duration<double>(now - closedSince).count(), closedUpdates, stats.gateCloses);
    }

    // Returns whether the hooks collect during the next frame: the gate is open and the smoothed
    // frame rate is below the engage rate. Only a frame they collected in may run extra cycles, so
    // above the engage rate every hook passes its call straight through.
    bool RunExtraCycles(JobList* jobs)
    {
        const std::lock_guard lock(stateLock);
        const auto now = Clock::now();
        const auto last = std::exchange(previousCycle, now);
        const bool gateOpen = gameplayGate && gameplayGate();
        if (!gateOpen || !Chain::HooksIntact()) {
            if (gateOpen) {
                closedSince = {};
            } else {
                if (last != Clock::time_point{}) {
                    ++stats.gateCloses;
                }
                ReportClosedGate(now);
            }
            policy.Reset();
            previousCycle = {};
            return false;
        }
        closedSince = {};
        // The first frame after the gate opened has no interval; the policy takes zero as "none yet".
        const double intervalMS = last == Clock::time_point{} ? 0.0 :
            std::chrono::duration<double, std::milli>(now - last).count();
        // A frame the hooks did not collect in gets no extra cycles from the policy.
        const unsigned allowed = policy.BeginFrame(intervalMS, Chain::Active());
        const bool engaged = policy.Engaged();
        if (intervalMS <= 0.0 || intervalMS > CatchUpPolicy::maxIntervalMS) {
            // No interval yet, or a pause or load rather than a gameplay frame: the policy has reset.
            return engaged;
        }
        ++stats.frames;
        stats.intervalMS += intervalMS;
        if (intervalMS > policy.TargetMS()) {
            ++stats.slowFrames;
        }
        if (engaged) {
            ++stats.engagedFrames;
        }
        if (const auto load = loadedAt.load(std::memory_order_acquire); load != appliedLoad) {
            appliedLoad = load;
            settleUntil = Clock::time_point(Clock::duration(load)) + settleAfterLoad;
            pausedUntil = {};
        }
        // While held after a load or paused after an overrun, no extra cycle can run anyway; turn
        // hook collection off so the burst pays zero classification/lock overhead.
        if (now < settleUntil || now < pausedUntil) {
            policy.DropCredit();
            if (engaged) {
                ++stats.heldFrames;
            }
            MaybeLog(now);
            return false;
        }
        if (allowed == 0) {
            MaybeLog(now);
            return engaged;
        }

        auto* skyrimVM = RE::SkyrimVM::GetSingleton();
        auto* vm = VirtualMachine::GetSingleton();
        if (!skyrimVM || !vm) {
            policy.DropCredit();
            return false;
        }

        // Checked before anything reads the queue: the engine empties and reloads it without
        // funcQueueLock when it drops or loads the VM's running data (IDs 104834, 104896).
        if (Frozen(skyrimVM, vm)) {
            policy.DropCredit();
            ++stats.frozenStops;
            MaybeLog(now);
            return false;
        }

        // An extra cycle starts only when the call at the front of the queue may run. Otherwise its
        // Update would run no call, and the cycle would only repeat the VM's housekeeping stages and
        // continue tasklets that are waiting for the next frame.
        auto pending = ReadPending(vm);
        if (pending.front != Chain::Front::kMayRun || !cyclesAllowed.load(std::memory_order_relaxed)) {
            policy.Idle();
            ++(pending.front == Chain::Front::kMustWait ? stats.blockedSkips : stats.idleStops);
            MaybeLog(now);
            return engaged;
        }

        const float passBudget = PassBudget();
        const auto deadline = now + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(settings.maxExtraMS));
        // Releases the references to the function objects the frame's batches ran on any exit path.
        struct FrameRecordGuard
        {
            ~FrameRecordGuard() noexcept
            {
                frameCycles.clear();
                frameFunctions.clear();
            }
        } clearFrameRecords;
        unsigned ran = 0;
        while (ran < allowed) {
            if (ran > 0) {
                if (Clock::now() >= deadline) {
                    ++stats.timeStops;
                    break;
                }
                // A save or load can begin while the frame is running.
                if (Frozen(skyrimVM, vm)) {
                    policy.DropCredit();
                    ++stats.frozenStops;
                    break;
                }
                pending = ReadPending(vm);
                if (pending.front != Chain::Front::kMayRun) {
                    // Nothing queued, or the front call and every call behind it wait for the next
                    // stock update, as without the plugin.
                    policy.Idle();
                    ++(pending.front == Chain::Front::kMustWait ? stats.blockedStops : stats.idleStops);
                    break;
                }
            }
            stats.leftoverTasklets += pending.tasklets;
            // The VM half of the stock job in its order: Update, TasksToJobs, submit, wait, reset.
            // SkyrimVM's timer and event queues (Utility.Wait, OnUpdate, LOS) stay once per frame.
            // Each Update stage stops after the call that uses up its share of this budget, so the
            // budget is limited to the time left. Tasklet time is bounded by the engine's per-run op limit.
            const double leftMS = std::chrono::duration<double, std::milli>(deadline - Clock::now()).count();
            const float budget = std::max(minimumUpdateMS, std::min(passBudget, static_cast<float>(leftMS)));
            const auto updateStart = Clock::now();
            Chain::inExtraCycle = true;
            Chain::blockedInCycle = false;
            Chain::movedBehind.clear();
            vm->Update(budget);
            Chain::inExtraCycle = false;
            Chain::RestoreQueueOrder(vm);
            const bool blocked = Chain::blockedInCycle;
            const auto updateEnd = Clock::now();
            const double updateMS = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
            stats.maxUpdateMS = std::max(stats.maxUpdateMS, updateMS);
            policy.Consume();
            ++ran;
            const auto firstFunction = frameFunctions.size();
            CycleRecord record{ updateMS, 0.0, 0, firstFunction, 0 };
            if (const auto tasklets = TakeWaitingTasklets(vm); tasklets != 0) {
                vm->TasksToJobs(*jobs);
                reinterpret_cast<SubmitFn*>(submitAddress)(jobs, nullptr, 0);
                waitAddress(jobs);
                originalReset(jobs);
                ++stats.taskletCycles;
                const double batchMS = std::chrono::duration<double, std::milli>(Clock::now() - updateEnd).count();
                if (batchMS > stats.maxBatchMS) {
                    stats.maxBatchMS = batchMS;
                    stats.slowestBatchTasklets = tasklets;
                }
                stats.maxBatchTasklets = std::max(stats.maxBatchTasklets, tasklets);
                record.batchMS = batchMS;
                record.tasklets = tasklets;
            }
            record.functions = frameFunctions.size() - firstFunction;
            if (frameCycles.size() < frameCycles.capacity()) {
                frameCycles.push_back(record);
            }
            if (Chain::disabled.load(std::memory_order_relaxed)) {
                // A rule violation in this cycle turned extra cycles off; it is counted as a
                // violation, not as a stop the rule asked for.
                policy.DropCredit();
                break;
            }
            if (blocked) {
                // A call that may not run in this frame reached the front; it and every call behind
                // it wait for the next stock update, as without the plugin.
                policy.Idle();
                ++stats.blockedStops;
                break;
            }
            // Otherwise the next pass looks at the front again before it starts another cycle.
        }

        const auto end = Clock::now();
        const double extraMS = std::chrono::duration<double, std::milli>(end - now).count();
        const bool paused = extraMS > settings.maxExtraMS * overrunFactor;
        if (paused) {
            pausedUntil = end + overrunPause;
            policy.DropCredit();
            ++stats.overrunPauses;
            SKSE::log::info("VM catch-up paused for {} s: {} extra cycles took {:.3f} ms (limit {:.2f} ms)",
                overrunPause.count(), ran, extraMS, settings.maxExtraMS);
            ReportSlowFrame();
        }
        frameCycles.clear();
        frameFunctions.clear();
        stats.cycles += ran;
        stats.extraMS += extraMS;
        stats.maxFrameExtraMS = std::max(stats.maxFrameExtraMS, extraMS);
        MaybeLog(end);
        return engaged && !paused;
    }

    void ResetGameplayJobs(JobList* jobs)
    {
        originalReset(jobs);
        const bool collect = RunExtraCycles(jobs);
        // Calls left in the queue run in the next stock update.
        Chain::EndFrame(collect);
    }
}

VMCatchUp::Settings VMCatchUp::Sanitize(Settings value)
{
    const Settings defaults{};
    if (!std::isfinite(value.targetHz) || value.targetHz < CatchUpPolicy::minTargetHz ||
        value.targetHz > CatchUpPolicy::maxTargetHz) {
        value.targetHz = defaults.targetHz;
    }
    // Kept as requested; the policy uses min(engageHz, targetHz).
    if (!std::isfinite(value.engageHz) || value.engageHz < CatchUpPolicy::minTargetHz ||
        value.engageHz > CatchUpPolicy::maxTargetHz) {
        value.engageHz = defaults.engageHz;
    }
    if (value.maxPasses < 1 || value.maxPasses > CatchUpPolicy::maxPassLimit) {
        value.maxPasses = defaults.maxPasses;
    }
    if (!std::isfinite(value.maxExtraMS) || value.maxExtraMS < 0.25 || value.maxExtraMS > 8.0) {
        value.maxExtraMS = defaults.maxExtraMS;
    }
    return value;
}

bool VMCatchUp::Install(const Settings& requested, GateFn* gate)
{
    if (installed || !gate) {
        return installed;
    }
    const auto job = REL::ID(39074).address();
    std::vector<const char*> chained;
    bool jobHooked = false;
    if (!MatchesCode(job + jobTailOffset, jobTail, jobTailCalls, jobHooked)) {
        SKSE::log::warn("VM catch-up disabled because the gameplay VM job differs from AE 1.6.1170 or was patched");
        return false;
    }
    // Extra cycles are only installed together with the queue hooks that enforce the mode's rule.
    const auto sanitized = Sanitize(requested);
    const Chain::Addresses chainAt{ job, REL::ID(104767).address(), REL::ID(104853).address(),
        REL::ID(104857).address(), REL::ID(104858).address(), REL::ID(105176).address(), REL::ID(105204).address() };
    if (jobHooked) {
        chained.push_back("gameplay VM job");
    }
    if (!Chain::Verify(sanitized.mode, sanitized.observeQueuedCalls, chainAt, chained)) {
        SKSE::log::warn("VM catch-up disabled because the native-call queue code differs from AE 1.6.1170 or was patched");
        return false;
    }
    {
        const std::lock_guard lock(stateLock);
        settings = sanitized;
        policy.Configure(settings.targetHz, settings.maxPasses, settings.engageHz);
        gameplayGate = gate;
        stats = {};
        previousCycle = {};
        nextLog = Clock::now() + std::chrono::seconds(settings.logIntervalSeconds);
        // Capacity once, so that recording a frame never allocates (see frameFunctions).
        frameCycles.reserve(CatchUpPolicy::maxPassLimit);
        frameFunctions.reserve(maxRecordedFunctions);
        // The same targets the stock job calls, so other plugins' hooks on them still run.
        submitAddress = reinterpret_cast<JobsFn*>(Chain::CallTarget(job + jobTailOffset + jobTailCalls[2]));
        waitAddress = reinterpret_cast<JobsFn*>(Chain::CallTarget(job + jobTailOffset + jobTailCalls[3]));
        updateBudgetMS = reinterpret_cast<const float*>(REL::ID(383725).address());
        Chain::Install(settings.mode, settings.observeQueuedCalls, chainAt);
        originalReset = SKSE::GetTrampoline().write_call<5>(job + resetCallOffset, ResetGameplayJobs);
        Chain::Record(job + resetCallOffset, "gameplay VM job reset");
        installed = true;
    }
    SKSE::log::info("Installed VM catch-up: mode={}, below {:.0f} FPS, target={:.0f} Hz, max {} extra cycles and {:.2f} ms per frame, skip waiting calls={}",
        settings.mode == Mode::kAccelerate ? "accelerate" : "carried", std::min(settings.engageHz, settings.targetHz),
        settings.targetHz, settings.maxPasses, settings.maxExtraMS,
        settings.mode == Mode::kAccelerate && Chain::skipBlocked.load(std::memory_order_relaxed));
    for (const auto* name : chained) {
        SKSE::log::info("VM catch-up: {} call was already hooked by another plugin; calls are passed through to it", name);
    }
    return true;
}

void VMCatchUp::NotifyGameLoaded()
{
    // Nonzero even at the clock's epoch, so a load is never mistaken for "no load yet".
    loadedAt.store(std::max<Clock::rep>(Clock::now().time_since_epoch().count(), 1), std::memory_order_release);
    Chain::Reset();
}

bool VMCatchUp::Installed()
{
    const std::lock_guard lock(stateLock);
    return installed;
}

void VMCatchUp::SetTargetHz(double targetHz)
{
    const std::lock_guard lock(stateLock);
    auto requested = settings;
    requested.targetHz = targetHz;
    settings = Sanitize(requested);
    policy.Configure(settings.targetHz, settings.maxPasses, settings.engageHz);
}

void VMCatchUp::SetCyclesAllowed(bool allowed)
{
    cyclesAllowed.store(allowed, std::memory_order_relaxed);
}

void VMCatchUp::SetDirectJudgement(bool enabled)
{
    Chain::directJudgement.store(enabled, std::memory_order_relaxed);
}

void VMCatchUp::SetExclusionList(bool enabled)
{
    Chain::exclusionList.store(enabled, std::memory_order_relaxed);
}

void VMCatchUp::SetHoldChangeCalls(bool enabled)
{
    Chain::holdChangeCalls.store(enabled, std::memory_order_relaxed);
}

void VMCatchUp::SetReadRule(ReadRule rule)
{
    Chain::readRule.store(rule, std::memory_order_relaxed);
}

void VMCatchUp::SetPropertyReads(bool enabled)
{
    Chain::propertyReads.store(enabled, std::memory_order_relaxed);
}

void VMCatchUp::SetSkipBlocked(bool enabled)
{
    Chain::skipBlocked.store(enabled, std::memory_order_relaxed);
}

void VMCatchUp::SetCensus(bool enabled)
{
    Chain::census.store(enabled, std::memory_order_relaxed);
}

std::vector<VMCatchUp::CensusEntry> VMCatchUp::TakeCensus()
{
    std::vector<CensusEntry> result;
    const std::lock_guard guard(Chain::censusLock);
    result.reserve(Chain::censusCounts.size());
    for (auto& [key, entry] : Chain::censusCounts) {
        result.push_back(std::move(entry));
    }
    Chain::censusCounts.clear();
    return result;
}

std::vector<std::pair<std::string, std::uint64_t>> VMCatchUp::TakeBlockers()
{
    const std::lock_guard guard(Chain::lock);
    std::vector<std::pair<std::string, std::uint64_t>> result(Chain::blockerCounts.begin(), Chain::blockerCounts.end());
    Chain::blockerCounts.clear();
    return result;
}

VMCatchUp::Stats VMCatchUp::ReadAndResetStats()
{
    const std::lock_guard lock(stateLock);
    auto result = std::exchange(stats, {});
    result.guardViolations = Chain::violations.exchange(0, std::memory_order_relaxed);
    result.extraCalls = Chain::extraCalls.exchange(0, std::memory_order_relaxed);
    result.directChanges = Chain::directChanges.exchange(0, std::memory_order_relaxed);
    result.changeCallHolds = Chain::changeCallHolds.exchange(0, std::memory_order_relaxed);
    result.frontSkips = Chain::frontSkips.exchange(0, std::memory_order_relaxed);
    result.orderRebuilds = Chain::orderRebuilds.exchange(0, std::memory_order_relaxed);
    return result;
}
