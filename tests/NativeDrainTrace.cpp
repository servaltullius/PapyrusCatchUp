#include "NativeDrainTrace.h"
#include "Continuation.h"
#include "QueueHeadroom.h"

#include "RE/B/BSJobs.h"
#include "RE/F/FunctionMessage.h"
#include "RE/O/Object.h"
#include "RE/S/Stack.h"
#include "RE/S/StackFrame.h"
#include "RE/S/State.h"

#include <Windows.h>
#include <limits>

#undef GetObject

namespace
{
    using VM = RE::BSScript::Internal::VirtualMachine;
    using Message = RE::BSScript::Internal::FunctionMessage;
    using Stack = RE::BSScript::Stack;
    using Function = RE::BSScript::IFunction;
    using Query = RE::BSTSmartPointer<RE::BSScript::Internal::IFuncCallQuery>;
    using JobList = RE::BSJobs::JobList;
    using JobListLocal = RE::BSJobs::JobListLocal;

    REL::Relocation<void(*)(VM*, float, std::uint32_t*, std::uint32_t*)> originalDrain;
    REL::Relocation<bool(*)(VM*, Message*)> originalPop;
    REL::Relocation<bool(*)(VM*, Stack*, const Query&)> originalEnqueue;
    REL::Relocation<std::uint64_t(*)()> originalClock;
    REL::Relocation<void(*)(VM*, float)> originalStage859;
    REL::Relocation<void(*)(VM*, float, std::uint32_t*)> originalStage860;
    REL::Relocation<void(*)(VM*, float)> originalUpdate;
    REL::Relocation<void(*)(VM*, float)> originalUpdateTasklets;
    REL::Relocation<void(*)(JobList*, JobList*, bool)> originalSubmitJobs;
    REL::Relocation<void(*)(JobList*)> originalWaitJobs;
    REL::Relocation<void(*)(VM*, JobList&)> originalCollectJobs;
    std::uintptr_t taskletJobCallback{};
    ResumeTrace::IsCapturing* isCapturing{};
    ResumeTrace::Record* record{};
    bool compareBudget{};
    bool observeFinish{};
    bool observeBatch{};
    std::atomic_bool armed{}, enabled{};
    std::atomic_uint32_t usedFrame{};
    std::atomic_uint64_t previousDrain{};
    std::array<std::atomic_uint32_t, 64> targets{};
    std::uint64_t minimumInterval{}, maximumInterval{};
    float clockTicksPerMS{};
    constexpr float extraBudgetMS = 0.25f;

    struct Phase
    {
        std::uint64_t decision{};
        std::uint64_t poppedAt{};
        RE::VMStackID stack{};
        std::uint32_t type{};
        bool empty{};
        float baseBudget{};
        std::uint64_t baseDeadline{};
        bool extended{};
        bool overBase{};
    };

    thread_local Phase* activePhase{};

    struct TaskletPublication
    {
        VM* vm{};
        RE::VMStackID stack{};
        const RE::BSScript::Internal::IFuncCallQuery* query{};
        std::uint64_t taskletStart{};
    };

    thread_local TaskletPublication taskletPublication{};
    thread_local JobListLocal* submittedBatch{};

    struct HeadSnapshot
    {
        RE::VMStackID stack{};
        std::uint32_t kind{};
        std::uint32_t frame{};
        bool exactTarget{};
        bool rootLockHeld{};
        std::uintptr_t queryIdentity{};
    };

    std::uint64_t Now()
    {
        LARGE_INTEGER time{};
        QueryPerformanceCounter(&time);
        return time.QuadPart;
    }

    std::uint32_t RenderFrame()
    {
        return static_cast<volatile const std::uint32_t&>(RE::BSGraphics::State::GetSingleton()->GetFrameCount());
    }

    std::uint32_t NativeContext(const VM& vm)
    {
        const auto* freeze = reinterpret_cast<const volatile std::uint32_t*>(&vm.frozenStacksLock);
        const bool ownsFreeze = freeze[0] == GetCurrentThreadId() && freeze[1] != 0;
        return (ownsFreeze ? 4u : 0u) | (activePhase ? 8u : 0u);
    }

    bool Enqueue(VM* vm, Stack* stack, const Query& query)
    {
        if (!stack || !isCapturing()) {
            return originalEnqueue(vm, stack, query);
        }
        const auto id = stack->stackID;
        const auto depth = stack->frames;
        record('[', id, depth, nullptr, nullptr, nullptr, ResumeTrace::TaskletStartedAt(stack));
        const auto result = originalEnqueue(vm, stack, query);
        record(']', id, depth, nullptr, nullptr, nullptr, result);
        return result;
    }

    bool LockHeldBy(const Stack& stack)
    {
        const auto& self = stack.top->self;
        if (!self.IsObject()) {
            return false;
        }
        const auto object = self.GetObject();
        if (!object) {
            return false;
        }
        const auto owner = reinterpret_cast<std::uintptr_t>(static_cast<void* volatile const&>(object->lockStructure));
        return (owner & ~std::uintptr_t{ 1 }) == reinterpret_cast<std::uintptr_t>(&stack);
    }

    void ClearTarget(RE::VMStackID id)
    {
        targets[id % targets.size()].compare_exchange_strong(id, 0, std::memory_order_relaxed);
    }

    bool TaggedEnqueue(VM* vm, Stack* stack, const Query& query, const Function* function)
    {
        if ((compareBudget && !armed.load(std::memory_order_relaxed)) || !isCapturing() || !stack || stack->frames != 1 || !function ||
            !Continuation::IsStockWorldSpace(*function) || !Continuation::IsDynDOLODRoot(stack->top) || !LockHeldBy(*stack)) {
            return Enqueue(vm, stack, query);
        }
        const auto id = stack->stackID;
        if (observeFinish) {
            const auto start = ResumeTrace::TaskletStartedAt(stack);
            const auto frame = RenderFrame();
            const auto* queryIdentity = query.get();
            const auto result = Enqueue(vm, stack, query);
            if (result && start) {
                taskletPublication = { vm, id, queryIdentity, start };
                record('t', id, 1, nullptr, nullptr, nullptr, frame);
            }
            return result;
        }
        auto& slot = targets[id % targets.size()];
        std::uint32_t empty{};
        const bool tagged = id && slot.compare_exchange_strong(empty, id, std::memory_order_relaxed);
        if (tagged) {
            record('t', id, stack->frames, nullptr, nullptr, nullptr, 0);
        }
        const auto result = Enqueue(vm, stack, query);
        if (!result && tagged) {
            ClearTarget(id);
        }
        return result;
    }

    bool Pop(VM* vm, Message* message)
    {
        const auto result = originalPop(vm, message);
        if (auto* phase = activePhase) {
            const auto time = Now();
            if (result) {
                phase->poppedAt = time;
                phase->stack = message->stack ? message->stack->stackID : 0;
                phase->type = static_cast<std::uint32_t>(message->type);
                if (compareBudget && phase->stack && message->type == Message::Type::kCall) {
                    ClearTarget(phase->stack);
                }
            } else {
                phase->empty = true;
                phase->decision = time;
            }
        }
        return result;
    }

    std::uint64_t StartClock()
    {
        const auto time = originalClock();
        if (auto* phase = activePhase; phase && phase->extended) {
            phase->baseDeadline = time + static_cast<std::uint64_t>(phase->baseBudget * clockTicksPerMS);
        }
        return time;
    }

    std::uint64_t CheckBudget()
    {
        const auto time = originalClock();
        if (auto* phase = activePhase) {
            phase->decision = Now();
            // The stock drain has already released the queue lock at this point.
            record('K', phase->stack, phase->type, nullptr, nullptr, nullptr, phase->poppedAt);
            if (phase->extended && phase->overBase) {
                record('=', phase->stack, phase->type, nullptr, nullptr, nullptr, phase->poppedAt);
            }
            phase->overBase = phase->extended && time > phase->baseDeadline;
        }
        return time;
    }

    std::pair<RE::VMStackID, unsigned> ReadyTarget(VM& vm)
    {
        const RE::BSSpinLockGuard guard(vm.funcQueueLock);
        if (!vm.overflowFuncMsgs.empty()) {
            return {};
        }
        auto* node = vm.funcMsgQueue.head;
        for (unsigned rank = 0; node && rank < 48; ++rank, node = node->next) {
            const auto& message = *reinterpret_cast<const Message*>(node->elem);
            const auto* stack = message.stack.get();
            if (message.type == Message::Type::kCall && stack && stack->stackID &&
                targets[stack->stackID % targets.size()].load(std::memory_order_relaxed) == stack->stackID) {
                return { stack->stackID, rank };
            }
        }
        return {};
    }

    void ObserveStage859(VM* vm, float budget)
    {
        const bool capturing = isCapturing();
        if (capturing) {
            record('g', RenderFrame(), 859, nullptr, nullptr, nullptr, 0);
        }
        originalStage859(vm, budget);
        if (capturing) {
            record('l', RenderFrame(), 859, nullptr, nullptr, nullptr, 0);
        }
    }

    HeadSnapshot QueueHead(VM& vm, const TaskletPublication* target = nullptr, bool inspectBatchHead = false)
    {
        const RE::BSSpinLockGuard guard(vm.funcQueueLock);
        HeadSnapshot head;
        if (!vm.overflowFuncMsgs.empty()) {
            head.kind = std::numeric_limits<std::uint32_t>::max();
        } else if (const auto* node = vm.funcMsgQueue.head) {
            const auto& message = *reinterpret_cast<const Message*>(node->elem);
            const auto* stack = message.stack.get();
            head.stack = stack ? stack->stackID : 0;
            head.kind = static_cast<std::uint32_t>(message.type);
            head.queryIdentity = reinterpret_cast<std::uintptr_t>(message.funcQuery.get());
            head.exactTarget = target && stack && head.stack == target->stack && message.type == Message::Type::kCall &&
                message.funcQuery.get() == target->query;
            const bool inspectOwner = head.exactTarget || (inspectBatchHead && stack && message.type == Message::Type::kCall &&
                Continuation::IsDynDOLODRoot(stack->top));
            head.rootLockHeld = inspectOwner && stack->frames == 1 && stack->top && LockHeldBy(*stack);
        }
        head.frame = RenderFrame();
        return head;
    }

    void ObserveStage860(VM* vm, float budget, std::uint32_t* pending)
    {
        const bool capturing = isCapturing();
        if (capturing) {
            record('g', RenderFrame(), 860, nullptr, nullptr, nullptr, 0);
        }
        originalStage860(vm, budget, pending);
        if (capturing) {
            record('l', RenderFrame(), 860, nullptr, nullptr, nullptr, 0);
            const auto head = QueueHead(*vm);
            record('F', head.stack, head.kind, nullptr, nullptr, nullptr, head.frame);
        }
    }

    void ObserveUpdate(VM* vm, float budget)
    {
        const bool capturing = isCapturing();
        if (capturing) {
            record('g', RenderFrame(), 767, nullptr, nullptr, nullptr, 0);
        }
        originalUpdate(vm, budget);
        if (capturing) {
            record('l', RenderFrame(), 767, nullptr, nullptr, nullptr, 0);
        }
    }

    void ObserveUpdateTasklets(VM* vm, float budget)
    {
        const bool capturing = isCapturing();
        if (capturing) {
            record('g', RenderFrame(), 768, nullptr, nullptr, nullptr, 0);
        }
        originalUpdateTasklets(vm, budget);
        if (capturing) {
            record('l', RenderFrame(), 768, nullptr, nullptr, nullptr, 0);
            const auto head = QueueHead(*vm);
            const auto context = std::uint64_t{ NativeContext(*vm) } << 32;
            record('J', head.stack, head.kind, nullptr, nullptr, nullptr, context | head.frame);
        }
    }

    void ObserveSubmitJobs(JobList* list, JobList* dependency, bool synchronous)
    {
        submittedBatch = nullptr;
        auto* jobs = list ? list->jobList : nullptr;
        if (isCapturing() && jobs && !jobs->jobArray.empty()) {
            const auto isTasklet = [](const auto& job) {
                return reinterpret_cast<std::uintptr_t>(job.func) == taskletJobCallback && job.data;
            };
            const auto count = std::ranges::count_if(jobs->jobArray, isTasklet);
            if (count) {
                submittedBatch = jobs;
                record('{', RenderFrame(), static_cast<std::uint32_t>(count), nullptr, nullptr, nullptr,
                    reinterpret_cast<std::uintptr_t>(jobs));
                for (const auto& job : jobs->jobArray) {
                    if (isTasklet(job)) {
                        record('q', 0, 0, nullptr, nullptr, nullptr, reinterpret_cast<std::uintptr_t>(job.data));
                    }
                }
            }
        }
        originalSubmitJobs(list, dependency, synchronous);
    }

    void ObserveWaitJobs(JobList* list, VM* vm)
    {
        auto* jobs = std::exchange(submittedBatch, nullptr);
        if (!jobs || !list || list->jobList != jobs || !isCapturing()) {
            originalWaitJobs(list);
            return;
        }
        const auto identity = reinterpret_cast<std::uintptr_t>(jobs);
        const auto version = std::atomic_ref(jobs->version.load_unchecked()).load(std::memory_order_acquire);
        record('|', RenderFrame(), jobs->signalJobCountArray.size(), nullptr, nullptr, nullptr, identity);
        originalWaitJobs(list);
        const auto after = std::atomic_ref(jobs->version.load_unchecked()).load(std::memory_order_acquire);
        const auto signedUp = std::atomic_ref(jobs->numThreadsSignedUp.load_unchecked()).load(std::memory_order_acquire);
        const bool completed = after == version + 1 && signedUp == 0 && (jobs->flags & 2);
        record('}', RenderFrame(), completed, nullptr, nullptr, nullptr, identity);
        if (completed && vm) {
            const auto head = QueueHead(*vm, nullptr, true);
            const auto flags = NativeContext(*vm) | (head.rootLockHeld ? 2u : 0u);
            record('%', head.stack, flags, nullptr, nullptr, nullptr, head.frame);
            record('^', head.stack, head.kind, nullptr, nullptr, nullptr, head.queryIdentity);
        }
    }

    void ObserveCollectJobs(VM* vm, JobList& list)
    {
        if (!isCapturing() || !list.jobList) {
            originalCollectJobs(vm, list);
            return;
        }
        const auto& jobs = list.jobList->jobArray;
        record('g', jobs.size(), 855, nullptr, nullptr, nullptr, jobs.capacity());
        originalCollectJobs(vm, list);
        record('l', jobs.size(), 855, nullptr, nullptr, nullptr, jobs.capacity());
    }

    void Drain(VM* vm, float budget, std::uint32_t* running, std::uint32_t* pending)
    {
        std::uint64_t interval{};
        if (compareBudget) {
            const auto now = Now();
            interval = now - previousDrain.exchange(now, std::memory_order_relaxed);
        }
        if (!isCapturing()) {
            originalDrain(vm, budget, running, pending);
            return;
        }
        const auto frame = RenderFrame();
        record('D', frame, observeFinish ? NativeContext(*vm) : 0, nullptr, nullptr, nullptr, std::bit_cast<std::uint32_t>(budget));
        Phase phase;
        phase.baseBudget = budget;
        if (compareBudget && armed.load(std::memory_order_relaxed) && interval > minimumInterval && interval < maximumInterval &&
            std::isfinite(budget) && budget > 0.0f && budget < 2.0f && !vm->overstressed && !vm->frozenStacks &&
            vm->freezeState == VM::FreezeState::kNotFrozen) {
            if (const auto [target, rank] = ReadyTarget(*vm); target) {
                const bool reserve = QueueHeadroom::HasRecentReserve();
                record('V', target, rank, nullptr, nullptr, nullptr, reserve);
                if (reserve && enabled.load(std::memory_order_relaxed) && RenderFrame() == frame &&
                    usedFrame.exchange(frame, std::memory_order_relaxed) != frame) {
                    phase.extended = true;
                    record('W', target, rank, nullptr, nullptr, nullptr, frame);
                }
            }
        }
        auto* previous = std::exchange(activePhase, &phase);
        originalDrain(vm, budget + (phase.extended ? extraBudgetMS : 0.0f), running, pending);
        activePhase = previous;
        record('d', frame, phase.empty ? 0 : 1, nullptr, nullptr, nullptr, phase.decision);
    }
}

void NativeDrainTrace::ObserveTaskletEnd(std::uint64_t startedAt, std::uintptr_t returnAddress, std::uintptr_t taskletIdentity)
{
    if (!observeFinish || taskletPublication.taskletStart != startedAt) {
        return;
    }
    const auto target = std::exchange(taskletPublication, {});
    const auto& module = REL::Module::get();
    const auto& code = module.segment(REL::Segment::textx);
    const auto caller = returnAddress >= code.address() && returnAddress - code.address() < code.size() ? returnAddress - module.base() : 0;
    record('!', target.stack, RenderFrame(), nullptr, nullptr, nullptr, caller);
    const auto head = QueueHead(*target.vm, &target);
    const auto flags = NativeContext(*target.vm) | (head.exactTarget ? 1u : 0u) | (head.rootLockHeld ? 2u : 0u) |
        (head.kind == std::numeric_limits<std::uint32_t>::max() ? 16u : 0u);
    record('@', head.stack, flags, nullptr, nullptr, nullptr, head.frame);
    if (observeBatch) {
        record('Y', target.stack, 0, nullptr, nullptr, nullptr, taskletIdentity);
        record('?', target.stack, 0, nullptr, nullptr, nullptr, reinterpret_cast<std::uintptr_t>(target.query));
    }
}

void NativeDrainTrace::BeginTrial(bool active)
{
    armed.store(false, std::memory_order_relaxed);
    enabled.store(active, std::memory_order_relaxed);
    for (auto& target : targets) {
        target.store(0, std::memory_order_relaxed);
    }
}

void NativeDrainTrace::Arm(bool ready)
{
    armed.store(ready, std::memory_order_relaxed);
}

bool NativeDrainTrace::Install(ResumeTrace::IsCapturing* capturing, ResumeTrace::Record* recorder, bool compare, bool observeStages,
    bool finish, bool batch, bool releaseSource)
{
    const auto update = REL::ID(104767).address();
    const auto drainCall = update + 0xFF;
    const auto stage859 = update + 0x119;
    const auto stage860 = update + 0x128;
    const auto drain = REL::ID(104857).address();
    const auto firstPop = drain + 0xFA;
    const auto nextPop = drain + 0x1BF;
    const auto budgetClock = drain + 0x193;
    const auto startClock = drain + 0x57;
    const auto eligibility = REL::ID(104853).address() + 0x618;
    const auto enqueue = eligibility + 0x1B;
    constexpr std::array<std::uint8_t, 5> drainCode{ 0xE8, 0x0C, 0xB3, 0, 0 };
    constexpr std::array<std::uint8_t, 5> stage859Code{ 0xE8, 0xC2, 0xBB, 0, 0 };
    constexpr std::array<std::uint8_t, 5> stage860Code{ 0xE8, 0xD3, 0xBD, 0, 0 };
    constexpr std::array<std::uint8_t, 5> firstPopCode{ 0xE8, 0x41, 0x22, 0, 0 };
    constexpr std::array<std::uint8_t, 5> nextPopCode{ 0xE8, 0x7C, 0x21, 0, 0 };
    constexpr std::array<std::uint8_t, 5> clockCode{ 0xE8, 0x88, 0xAA, 0x87, 0xFF };
    constexpr std::array<std::uint8_t, 5> enqueueCode{ 0xE8, 0x38, 0xEF, 0xFF, 0xFF };
    constexpr std::array<std::uint8_t, 5> startClockCode{ 0xE8, 0xC4, 0xAB, 0x87, 0xFF };
    constexpr std::array<std::uint8_t, 8> clockFactorCode{ 0xF3, 0x0F, 0x10, 0x05, 0x7D, 0x9E, 0xBD, 0x00 };
    constexpr std::array<std::uint8_t, 18> functionSlotCode{
        0x48, 0x8B, 0x4C, 0x24, 0x50, 0x48, 0x8B, 0x01, 0xFF, 0x90, 0x98, 0, 0, 0, 0x84, 0xC0, 0x75, 0x13
    };
    if (!REL::verify_code(drainCall, drainCode.data(), drainCode.size()) ||
        !REL::verify_code(firstPop, firstPopCode.data(), firstPopCode.size()) ||
        !REL::verify_code(nextPop, nextPopCode.data(), nextPopCode.size()) ||
        !REL::verify_code(budgetClock, clockCode.data(), clockCode.size()) ||
        !REL::verify_code(enqueue, enqueueCode.data(), enqueueCode.size()) ||
        (observeStages && (!REL::verify_code(stage859, stage859Code.data(), stage859Code.size()) ||
            !REL::verify_code(stage860, stage860Code.data(), stage860Code.size())))) {
        return false;
    }

    REL::Relocation<std::uintptr_t> vmTable{ VM::VTABLE[0] };
    const auto updateTasklets = finish ? REL::ID(104768).address() : 0;
    const auto collectJobs = releaseSource ? REL::ID(104855).address() : 0;
    if (finish && (reinterpret_cast<const std::uintptr_t*>(vmTable.address())[4] != update ||
            reinterpret_cast<const std::uintptr_t*>(vmTable.address())[5] != updateTasklets)) {
        return false;
    }
    if (releaseSource && reinterpret_cast<const std::uintptr_t*>(vmTable.address())[0x12] != collectJobs) {
        return false;
    }
    auto& trampoline = SKSE::GetTrampoline();
    std::array<std::uintptr_t, 2> submitSites{}, waitSites{};
    void* waitStub = nullptr;
    if (batch) {
        const auto game = REL::ID(39074).address();
        const auto menu = REL::ID(39075).address();
        // Both stock callers retain the same VM in RBX across the submit/wait pair.
        constexpr std::array<std::uint8_t, 13> gameCalls{
            0xE8, 0x97, 0x3A, 0x62, 0, 0x48, 0x8B, 0xCF, 0xE8, 0xAF, 0x3A, 0x62, 0
        };
        constexpr std::array<std::uint8_t, 13> menuCalls{
            0xE8, 0xBF, 0x39, 0x62, 0, 0x48, 0x8B, 0xCF, 0xE8, 0xD7, 0x39, 0x62, 0
        };
        constexpr std::array<std::uint8_t, 16> collectCode{
            0x4C, 0x8B, 0x03, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCB, 0x41, 0xFF, 0x90, 0x90, 0, 0, 0
        };
        submitSites = { game + 0x84, menu + 0x8C };
        waitSites = { game + 0x8C, menu + 0x94 };
        if (!REL::verify_code(submitSites[0], gameCalls.data(), gameCalls.size()) ||
            !REL::verify_code(submitSites[1], menuCalls.data(), menuCalls.size()) ||
            !REL::verify_code(game + 0x61, collectCode.data(), collectCode.size()) ||
            !REL::verify_code(menu + 0x69, collectCode.data(), collectCode.size())) {
            return false;
        }
        std::array<std::uint8_t, 17> stub{ 0x48, 0x8B, 0xD3, 0xFF, 0x25, 0, 0, 0, 0 };
        const auto handler = reinterpret_cast<std::uintptr_t>(ObserveWaitJobs);
        std::memcpy(stub.data() + 9, &handler, sizeof(handler));
        waitStub = trampoline.allocate(stub.size());
        std::memcpy(waitStub, stub.data(), stub.size());
        originalSubmitJobs = REL::ID(69352).address();
        originalWaitJobs = REL::ID(69353).address();
        taskletJobCallback = REL::ID(104901).address();
    }
    void* enqueueStub = nullptr;
    if ((compare || finish) && (!REL::verify_code(eligibility, functionSlotCode.data(), functionSlotCode.size()) ||
            !Continuation::VerifyWorldSpace())) {
        return false;
    }
    if (compare) {
        if (!REL::verify_code(startClock, startClockCode.data(), startClockCode.size()) ||
            !REL::verify_code(drain + 0x5F, clockFactorCode.data(), clockFactorCode.size())) {
            return false;
        }
        std::memcpy(&clockTicksPerMS, reinterpret_cast<const void*>(drain + 0x67 + 0xBD9E7D), sizeof(clockTicksPerMS));
        if (!std::isfinite(clockTicksPerMS) || clockTicksPerMS < 1000.0f || clockTicksPerMS > 1000000.0f ||
            !QueueHeadroom::InstallLimiter()) {
            return false;
        }
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        minimumInterval = frequency.QuadPart / 50;
        maximumInterval = frequency.QuadPart / 35;
    }
    if (compare || finish) {
        // R9 receives the stock IFunction at caller RSP+0x50, past the return address.
        std::array<std::uint8_t, 19> stub{ 0x4C, 0x8B, 0x4C, 0x24, 0x58, 0xFF, 0x25, 0, 0, 0, 0 };
        const auto handler = reinterpret_cast<std::uintptr_t>(TaggedEnqueue);
        std::memcpy(stub.data() + 11, &handler, sizeof(handler));
        enqueueStub = trampoline.allocate(stub.size());
        std::memcpy(enqueueStub, stub.data(), stub.size());
    }

    isCapturing = capturing;
    record = recorder;
    originalPop = REL::ID(104879).address();
    originalClock = REL::ID(68271).address();
    originalEnqueue = REL::ID(104848).address();
    compareBudget = compare;
    observeFinish = finish;
    observeBatch = batch;
    trampoline.write_call<5>(firstPop, Pop);
    trampoline.write_call<5>(nextPop, Pop);
    trampoline.write_call<5>(budgetClock, CheckBudget);
    if (compareBudget) {
        trampoline.write_call<5>(startClock, StartClock);
        SKSE::log::info("Installed guarded DynDOLOD native drain budget experiment, extra={}ms clock_ticks_per_ms={}",
            extraBudgetMS, clockTicksPerMS);
    }
    if (enqueueStub) {
        trampoline.write_call<5>(enqueue, reinterpret_cast<std::uintptr_t>(enqueueStub));
    } else {
        trampoline.write_call<5>(enqueue, Enqueue);
    }
    originalDrain = trampoline.write_call<5>(drainCall, Drain);
    if (observeStages) {
        originalStage859 = trampoline.write_call<5>(stage859, ObserveStage859);
        originalStage860 = trampoline.write_call<5>(stage860, ObserveStage860);
        SKSE::log::info("Installed stock post-drain stage and FIFO head observation");
    }
    if (observeFinish) {
        originalUpdate = update;
        originalUpdateTasklets = updateTasklets;
        vmTable.write_vfunc(4, ObserveUpdate);
        vmTable.write_vfunc(5, ObserveUpdateTasklets);
        SKSE::log::info("Installed stock tasklet completion, FIFO, and VM update context observation");
    }
    if (observeBatch) {
        for (std::size_t i = 0; i < submitSites.size(); ++i) {
            trampoline.write_call<5>(submitSites[i], ObserveSubmitJobs);
            trampoline.write_call<5>(waitSites[i], reinterpret_cast<std::uintptr_t>(waitStub));
        }
        SKSE::log::info("Installed stock Papyrus BSJobs submit, membership, and joined completion observation");
    }
    if (releaseSource) {
        originalCollectJobs = collectJobs;
        vmTable.write_vfunc(0x12, ObserveCollectJobs);
        SKSE::log::info("Installed stock Papyrus release source and job-array growth observation");
    }
    SKSE::log::info("Installed stock native drain observation with enqueue and pop boundaries");
    return true;
}
