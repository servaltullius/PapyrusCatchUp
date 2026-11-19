#include "ResumeTrace.h"
#include "Continuation.h"
#include "NativeDrainTrace.h"
#include "QueueHeadroom.h"

#include "RE/C/CodeTasklet.h"
#include "RE/N/NativeFunctionBase.h"
#include "RE/S/Stack.h"
#include "RE/S/StackFrame.h"
#include "RE/S/State.h"

#include <Windows.h>
#include <intrin.h>
#include <limits>

namespace
{
    using Native = RE::BSScript::NF_util::NativeFunctionBase;
    using VM = RE::BSScript::Internal::VirtualMachine;
    using Tasklet = RE::BSScript::Internal::CodeTasklet;
    using Value = RE::BSScript::Variable;
    using Frame = RE::BSScript::StackFrame;
    using Stack = RE::BSScript::Stack;
    using Object = RE::BSScript::Object;
    using Query = RE::BSScript::Internal::IFuncCallQuery;
    using QueryPtr = RE::BSTSmartPointer<Query>;
    using Suspended = RE::BSScript::Internal::SuspendedStack;

    ResumeTrace::IsCapturing* isCapturing{};
    ResumeTrace::Record* record{};
    REL::Relocation<void(*)(VM*, Tasklet*)> originalEnqueue;
    REL::Relocation<void(*)(Tasklet*, void*)> originalAcceptReturn;
    REL::Relocation<bool(*)(Object*, Stack*)> originalTryAcquire;
    REL::Relocation<void(*)(VM*, float, std::uint32_t*, std::uint32_t*)> originalDrain;
    REL::Relocation<void(*)(VM*, float, std::uint32_t*)> originalRetry;
    REL::Relocation<void(*)(Tasklet*)> originalTaskletProcess;
    REL::Relocation<void(*)(VM*, Stack*, const QueryPtr&, bool)> originalEnter;

    struct Waiter
    {
        RE::VMStackID id{};
        std::uint32_t frames{};
        const Object* object{};
        bool released{};
    };

    struct DeferredDrain
    {
        VM* vm{};
        float budget{};
        std::uint32_t* running{};
        std::uint32_t* pending{};
    };

    struct EntryWait
    {
        RE::VMStackID id{};
        std::uint32_t frames{};
        const Object* object{};
        const Query* query{};
        std::uint64_t missedAt{};
        std::uint32_t releaseFrame{};
        bool released{};
    };

    struct RetryPass
    {
        std::uint64_t start{};
        std::uint64_t interval{};
        std::uint32_t frame{};
    };

    class TryFreezeGuard
    {
    public:
        explicit TryFreezeGuard(RE::BSSpinLock& lock) : lock_(lock)
        {
            static_assert(sizeof(RE::BSSpinLock) == 2 * sizeof(LONG));
            // AE 1.6.1170's spin lock stores the owner ID followed by the count.
            auto* fields = reinterpret_cast<volatile LONG*>(&lock_);
            acquired_ = InterlockedCompareExchange(fields + 1, 1, 0) == 0;
            if (acquired_) {
                InterlockedExchange(fields, GetCurrentThreadId());
            }
        }

        ~TryFreezeGuard()
        {
            if (acquired_) {
                lock_.Unlock();
            }
        }

        explicit operator bool() const { return acquired_; }

    private:
        RE::BSSpinLock& lock_;
        bool acquired_{};
    };

    std::mutex waitLock;
    std::array<Waiter, 64> waiters{};
    std::array<EntryWait, 128> entryWaiters{};
    bool compareRetry{};
    bool compareEntry{};
    bool observeRetry{};
    bool observeReleaseSource{};
    bool retryFirst{};
    std::atomic_bool entryEnabled{}, entryOverrun{};
    std::atomic_uint64_t previousDrain{};
    std::uint64_t frameThreshold{}, slowFrameThreshold{};
    std::uint64_t entryLimitTicks{};
    RetryPass latestRetry;
    std::uint32_t lastAdvancedFrame{};
    thread_local DeferredDrain deferred;
    thread_local bool taskletReleasedWaiter{};
    thread_local std::pair<const Stack*, std::uint64_t> activeTasklet{};

    std::uint64_t QPC();

    std::uint32_t RenderFrame()
    {
        return static_cast<volatile const std::uint32_t&>(RE::BSGraphics::State::GetSingleton()->GetFrameCount());
    }

    void NotifyWaiters(const Object* object, bool released)
    {
        if (!compareRetry && !compareEntry) {
            return;
        }
        const std::lock_guard guard(waitLock);
        if (compareRetry) {
            for (auto& waiter : waiters) {
                if (waiter.object == object) {
                    waiter.released = released;
                }
            }
        } else {
            const auto frame = released ? RenderFrame() : 0;
            for (auto& waiter : entryWaiters) {
                if (waiter.object == object) {
                    waiter.released = released;
                    waiter.releaseFrame = frame;
                    taskletReleasedWaiter |= released;
                }
            }
        }
    }

    void ObserveAttempt(const Object* object, const Stack& stack, bool acquired)
    {
        if (!compareRetry) {
            return;
        }
        const std::lock_guard guard(waitLock);
        auto found = std::ranges::find(waiters, stack.stackID, &Waiter::id);
        if (acquired) {
            if (found != waiters.end()) {
                *found = {};
            }
        } else {
            if (found == waiters.end()) {
                found = std::ranges::find(waiters, 0u, &Waiter::id);
            }
            if (found != waiters.end()) {
                *found = { stack.stackID, stack.frames, object, false };
            }
        }
    }

    // Inspect the original queue while holding the same lock as the engine pop/swap.
    // Only advance a batch of return waiters whose oldest member saw an unlock.
    RE::VMStackID ReadyReturnHead(VM& vm)
    {
        const RE::BSSpinLockGuard queueGuard(vm.suspendQueueLock);
        const auto& queue = *vm.stacksToSuspend;
        if (!queue.numEntries || queue.numEntries > 8 || queue.popIdx >= 128 ||
            vm.stacksToResume->numEntries || !vm.stacksToResumeOverflow->empty() ||
            !vm.stacksToSuspendOverflow->empty()) {
            return 0;
        }
        const std::lock_guard guard(waitLock);
        RE::VMStackID head{};
        for (std::uint32_t i = 0; i < queue.numEntries; ++i) {
            using Suspended = RE::BSScript::Internal::SuspendedStack;
            const auto index = (queue.popIdx + i) % 128;
            const auto& entry = *reinterpret_cast<const Suspended*>(queue.queueBuffer + index * sizeof(Suspended));
            const auto* stack = entry.stack.get();
            if (!stack || entry.funcCallQuery ||
                (stack->state != Stack::State::kWaitingOnOtherStackForReturn &&
                    stack->state != Stack::State::kWaitingOnOtherStackForReturnNoPop)) {
                return 0;
            }
            const auto found = std::ranges::find(waiters, stack->stackID, &Waiter::id);
            if (found == waiters.end() || found->frames != stack->frames || (!i && !found->released)) {
                return 0;
            }
            if (!i) {
                head = stack->stackID;
            }
        }
        return head;
    }

    void Drain(VM* vm, float budget, std::uint32_t* running, std::uint32_t* pending)
    {
        const auto now = QPC();
        const auto interval = now - previousDrain.exchange(now, std::memory_order_relaxed);
        if (isCapturing() && interval > frameThreshold && interval < slowFrameThreshold &&
            !vm->overstressed && vm->freezeState == VM::FreezeState::kNotFrozen && !vm->frozenStacks) {
            if (const auto head = ReadyReturnHead(*vm)) {
                bool enabled;
                {
                    const std::lock_guard guard(waitLock);
                    enabled = retryFirst;
                }
                record(enabled ? 'd' : 'D', head, 0, nullptr, nullptr, nullptr, 0);
                if (enabled) {
                    // The immediately following VM call still performs the only retry pass.
                    deferred = { vm, budget, running, pending };
                    return;
                }
            }
        }
        originalDrain(vm, budget, running, pending);
    }

    void RecordRetryQueues(VM& vm, char marker)
    {
        std::uint32_t activeHead{}, nextHead{}, activeCount{}, nextCount{};
        {
            const RE::BSSpinLockGuard guard(vm.suspendQueueLock);
            const auto snapshot = [](const auto& queue, const auto& overflow, std::uint32_t& head, std::uint32_t& count) {
                if (!overflow.empty() || queue.numEntries > 128 || queue.popIdx >= 128) {
                    count = std::numeric_limits<std::uint32_t>::max();
                    return;
                }
                count = queue.numEntries;
                if (count) {
                    const auto* entry = reinterpret_cast<const Suspended*>(queue.queueBuffer + queue.popIdx * sizeof(Suspended));
                    if (entry->stack) {
                        head = entry->stack->stackID;
                    }
                }
            };
            snapshot(*vm.stacksToResume, *vm.stacksToResumeOverflow, activeHead, activeCount);
            snapshot(*vm.stacksToSuspend, *vm.stacksToSuspendOverflow, nextHead, nextCount);
        }
        record(marker, activeHead, activeCount, nullptr, nullptr, nullptr, (std::uint64_t{ nextCount } << 32) | nextHead);
    }

    void Retry(VM* vm, float budget, std::uint32_t* pending)
    {
        const auto drain = std::exchange(deferred, {});
        RetryPass pass;
        if (compareEntry && isCapturing()) {
            pass.start = QPC();
            pass.interval = pass.start - previousDrain.exchange(pass.start, std::memory_order_relaxed);
            pass.frame = RenderFrame();
        }
        const bool observing = observeRetry && isCapturing();
        if (observing) {
            RecordRetryQueues(*vm, 'J');
        }
        record('S', 0, 0, nullptr, nullptr, nullptr, observing ? std::bit_cast<std::uint32_t>(budget) : 0);
        originalRetry(vm, budget, pending);
        record('s', 0, 0, nullptr, nullptr, nullptr, 0);
        if (observing) {
            RecordRetryQueues(*vm, 'j');
        }
        if (pass.start) {
            const std::lock_guard guard(waitLock);
            latestRetry = pass;
        }
        if (drain.vm) {
            originalDrain(drain.vm, drain.budget, drain.running, drain.pending);
        }
    }

    std::uint64_t LockToken(const Object* owner)
    {
        return reinterpret_cast<std::uintptr_t>(owner);
    }

    // This call site has the native query in r12 and the original fixed-string
    // name followed by the requested ObjectTypeInfo in the caller's stack.
    bool TryAcquireEntryOwner(Object* owner, Stack* stack, const QueryPtr* query, const RE::BSFixedString* name)
    {
        const bool acquired = originalTryAcquire(owner, stack);
        if (!isCapturing() || !stack) {
            return acquired;
        }
        record(acquired ? 'C' : 'I', stack->stackID, stack->frames, nullptr, nullptr, name, LockToken(owner));
        const RE::BSScript::ObjectTypeInfo* type{};
        std::memcpy(&type, reinterpret_cast<const std::byte*>(name) + sizeof(*name), sizeof(type));
        const std::string_view className = type ? type->GetName() : "";
        const bool eligible = (className == "Actor" && (*name == "HasPerk" || *name == "IsSneaking")) ||
                              (className == "Form" && *name == "HasKeyword");
        const std::lock_guard guard(waitLock);
        auto found = std::ranges::find(entryWaiters, stack->stackID, &EntryWait::id);
        if (acquired || !eligible || !*query || !stack->frames) {
            if (found != entryWaiters.end()) {
                *found = {};
            }
            return acquired;
        }
        if (found == entryWaiters.end()) {
            found = std::ranges::find(entryWaiters, 0u, &EntryWait::id);
        }
        if (found != entryWaiters.end()) {
            *found = { stack->stackID, stack->frames, owner, query->get(), QPC() };
        }
        return acquired;
    }

    // Call with suspendQueueLock held. An unread resume batch or any overflow
    // takes precedence, and the next suspend ring's first entry is the only candidate.
    EntryWait EntryHead(const VM& vm, std::uint64_t retryStart)
    {
        const auto& queue = *vm.stacksToSuspend;
        if (!queue.numEntries || queue.popIdx >= 128 || vm.stacksToResume->numEntries ||
            !vm.stacksToResumeOverflow->empty() || !vm.stacksToSuspendOverflow->empty()) {
            return {};
        }
        const auto& head = *reinterpret_cast<const Suspended*>(queue.queueBuffer + queue.popIdx * sizeof(Suspended));
        const auto* stack = head.stack.get();
        if (!stack || !head.funcCallQuery || stack->state != Stack::State::kWaitingOnOtherStackForCall ||
            stack->freezeState != Stack::FreezeState::kUnfrozen) {
            return {};
        }
        const std::lock_guard guard(waitLock);
        const auto found = std::ranges::find(entryWaiters, stack->stackID, &EntryWait::id);
        if (found != entryWaiters.end() && found->frames == stack->frames && found->query == head.funcCallQuery.get() &&
            found->missedAt >= retryStart) {
            return *found;
        }
        return {};
    }

    void LateEntry(VM* vm)
    {
        const auto frame = RenderFrame();
        record('K', 0, 0, nullptr, nullptr, nullptr, frame);
        if (!QueueHeadroom::HasRecentReserve()) {
            return;
        }
        const TryFreezeGuard freeze(vm->frozenStacksLock);
        if (!freeze || vm->overstressed || vm->freezeState != VM::FreezeState::kNotFrozen || lastAdvancedFrame == frame) {
            return;
        }
        RetryPass pass;
        {
            const std::lock_guard guard(waitLock);
            pass = latestRetry;
        }
        if (!pass.start || pass.interval <= frameThreshold || pass.interval >= slowFrameThreshold || pass.frame != frame) {
            return;
        }
        record('V', 0, 0, nullptr, nullptr, nullptr, frame);

        const auto start = QPC();
        const bool enabled = entryEnabled.load(std::memory_order_relaxed) && !entryOverrun.load(std::memory_order_relaxed);
        Suspended entry;
        EntryWait head;
        {
            const RE::BSSpinLockGuard guard(vm->suspendQueueLock);
            head = EntryHead(*vm, pass.start);
            if (!head.id) {
                return;
            }
            record('J', head.id, head.frames, nullptr, nullptr, nullptr, frame);
            if (!head.released || head.releaseFrame != frame || RenderFrame() != frame) {
                return;
            }
            if (!enabled) {
                record('Y', head.id, head.frames, nullptr, nullptr, nullptr, frame);
                return;
            }
            if (QPC() - start >= entryLimitTicks || !vm->stacksToSuspend->TryPop(&entry)) {
                return;
            }
            lastAdvancedFrame = frame;
        }
        auto* stack = entry.stack.get();
        {
            const RE::BSSpinLockGuard guard(vm->runningStacksLock);
            const auto found = vm->allRunningStacks.find(stack->stackID);
            if (found == vm->allRunningStacks.end() || found->second.get() != stack) {
                return; // The stock suspended path also drops a removed stack.
            }
        }
        record('y', head.id, head.frames, nullptr, nullptr, nullptr, frame);
        stack->state = Stack::State::kRunning;
        originalEnter(vm, stack, entry.funcCallQuery, false);
        const auto spent = QPC() - start;
        record('j', head.id, head.frames, nullptr, nullptr, nullptr, spent);
        if (spent > entryLimitTicks) {
            entryOverrun.store(true, std::memory_order_relaxed);
        }
    }

    void ProcessTasklet(Tasklet* tasklet)
    {
        if (!isCapturing() || !tasklet) {
            originalTaskletProcess(tasklet);
            return;
        }
        auto* vm = tasklet->vm;
        const bool previous = std::exchange(taskletReleasedWaiter, false);
        originalTaskletProcess(tasklet);
        const bool released = std::exchange(taskletReleasedWaiter, previous);
        // The tasklet may be destroyed by the original. Only the VM is retained.
        if (released && vm && isCapturing()) {
            LateEntry(vm);
        }
    }

    void ObserveTasklet(Tasklet* tasklet)
    {
        if (!isCapturing() || !tasklet) {
            originalTaskletProcess(tasklet);
            return;
        }
        const auto started = QPC();
        const auto identity = reinterpret_cast<std::uintptr_t>(tasklet);
        const auto previous = std::exchange(activeTasklet, {tasklet->stack, started});
        originalTaskletProcess(tasklet);
        activeTasklet = previous;
        NativeDrainTrace::ObserveTaskletEnd(started, reinterpret_cast<std::uintptr_t>(_ReturnAddress()), identity);
    }

    bool AcquireSucceeded(Object* owner, Stack* stack)
    {
        // The original success path takes one intrusive reference.
        stack->IncRef();
        if (isCapturing()) {
            record('G', stack->stackID, 0, nullptr, nullptr, nullptr, LockToken(owner));
            NotifyWaiters(owner, false);
        }
        return true;
    }

    void ReleaseSucceeded(Object* owner, Stack* stack, std::uint32_t flags)
    {
        if (isCapturing()) {
            record(flags ? 'Z' : 'X', stack->stackID, 0, nullptr, nullptr, nullptr, LockToken(owner));
            if (observeReleaseSource && activeTasklet.first == stack && Continuation::IsDynDOLODRoot(stack->top)) {
                const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
                const auto& module = REL::Module::get();
                const auto& code = module.segment(REL::Segment::textx);
                const auto rva = caller >= code.address() && caller - code.address() < code.size() ? caller - module.base() : 0;
                record('~', stack->stackID, 0, nullptr, nullptr, nullptr, rva);
            }
            NotifyWaiters(owner, flags == 0);
        }
        // This path drops a reference without destroying the stack, as in the engine.
        stack->DecRef();
    }

    void LockFlagSet(Object* owner)
    {
        if (isCapturing()) {
            record('B', 0, 0, nullptr, nullptr, nullptr, LockToken(owner));
            NotifyWaiters(owner, false);
        }
    }

    template <std::size_t Size>
    void SetJump(std::array<std::uint8_t, Size>& stub, std::size_t offset, std::uintptr_t target)
    {
        const std::array<std::uint8_t, 6> opcode{ 0xFF, 0x25, 0, 0, 0, 0 };
        std::memcpy(stub.data() + offset, opcode.data(), opcode.size());
        std::memcpy(stub.data() + offset + opcode.size(), &target, sizeof(target));
    }

    bool InstallLockTransitions()
    {
        const auto acquire = REL::ID(104257).address();
        const auto release = REL::ID(104258).address();
        const auto flag = REL::ID(104259).address();
        constexpr std::array<std::uint8_t, 15> acquireExpected{
            0x33, 0xC0, 0xF0, 0x48, 0x0F, 0xB1, 0x51, 0x18, 0x75, 0x0B, 0x48, 0x85, 0xD2, 0x74, 0x03
        };
        constexpr std::array<std::uint8_t, 12> releaseExpected{
            0xF0, 0x49, 0x0F, 0xB1, 0x49, 0x18, 0x75, 0xE6, 0xF0, 0xFF, 0x0A, 0xC3
        };
        constexpr std::array<std::uint8_t, 5> flagExpected{ 0xC3, 0xCC, 0xCC, 0xCC, 0xCC };
        if (!REL::verify_code(acquire, acquireExpected.data(), acquireExpected.size()) ||
            !REL::verify_code(release + 0x22, releaseExpected.data(), releaseExpected.size()) ||
            !REL::verify_code(flag + 0x1F, flagExpected.data(), flagExpected.size())) {
            return false;
        }

        // The original acquire CAS is unchanged. Dispatch only its new-owner success path.
        std::array<std::uint8_t, 49> acquireStub{
            0x75, 0x21,              // jnz stock failure path
            0x48, 0x85, 0xD2,        // test rdx, rdx
            0x74, 0x0E               // jz stock null-owner success
        };
        SetJump(acquireStub, 7, reinterpret_cast<std::uintptr_t>(AcquireSucceeded));
        SetJump(acquireStub, 21, acquire + 0x12);
        SetJump(acquireStub, 35, acquire + 0x15);

        // Replay the original release CAS and retry branch. Preserve its resulting flag.
        std::array<std::uint8_t, 42> releaseStub{
            0xF0, 0x49, 0x0F, 0xB1, 0x49, 0x18, // lock cmpxchg [r9 + 0x18], rcx
            0x75, 0x14,                         // jnz stock retry
            0x44, 0x8B, 0xC1,                   // mov r8d, ecx
            0x49, 0x8B, 0xC9                    // mov rcx, r9
        };
        SetJump(releaseStub, 14, reinterpret_cast<std::uintptr_t>(ReleaseSucceeded));
        SetJump(releaseStub, 28, release + 0x10);

        auto& trampoline = SKSE::GetTrampoline();
        auto* acquireCode = trampoline.allocate(acquireStub.size());
        auto* releaseCode = trampoline.allocate(releaseStub.size());
        std::memcpy(acquireCode, acquireStub.data(), acquireStub.size());
        std::memcpy(releaseCode, releaseStub.data(), releaseStub.size());
        trampoline.write_branch<5>(acquire + 0x08, reinterpret_cast<std::uintptr_t>(acquireCode));
        trampoline.write_branch<5>(release + 0x22, reinterpret_cast<std::uintptr_t>(releaseCode));
        trampoline.write_branch<5>(flag + 0x1F, LockFlagSet);
        return true;
    }

    std::uint64_t QPC()
    {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return counter.QuadPart;
    }

    bool Marshal(const Native* function, Value& self, VM& vm, RE::VMStackID id, Value& result, const Frame& frame)
    {
        if (!isCapturing() || !frame.parent || frame.parent->frames == 0) {
            return function->MarshallAndDispatch(self, vm, id, result, frame);
        }
        const auto depth = frame.parent->frames - 1;
        const auto* type = &function->GetObjectTypeName();
        const auto* state = &function->GetStateName();
        const auto* name = &function->GetName();
        record('M', id, depth, type, state, name, 0);
        Continuation::ObserveNative(*function, frame, true);
        const auto start = QPC();
        const bool ok = function->MarshallAndDispatch(self, vm, id, result, frame);
        const auto ticks = QPC() - start;
        if (!ok) {
            Continuation::ObserveNative(*function, frame, false);
        }
        record(ok ? 'N' : 'n', id, depth, type, state, name, ticks);
        if (ok) {
            Continuation::ObserveResult(*function, frame, self, result);
        }
        return ok;
    }

    std::pair<RE::VMStackID, std::uint32_t> MarkTasklet(char event, const Tasklet* tasklet)
    {
        if (isCapturing() && tasklet && tasklet->stack &&
            tasklet->resumeReason == Tasklet::ResumeReason::kFunctionReturn) {
            const auto* stack = tasklet->stack;
            const auto depth = stack->frames ? stack->frames - 1 : 0;
            const auto detail = observeReleaseSource ? reinterpret_cast<std::uintptr_t>(tasklet) : stack->state.underlying();
            record(event, stack->stackID, depth, nullptr, nullptr, nullptr, detail);
            return {stack->stackID, depth};
        }
        return {};
    }

    void Enqueue(VM* vm, Tasklet* tasklet)
    {
        // This is the stock function-return caller. Mark before publishing the tasklet.
        const auto [stack, depth] = MarkTasklet('R', tasklet);
        const auto identity = reinterpret_cast<std::uintptr_t>(tasklet);
        Continuation::ObserveReturn(tasklet);
        originalEnqueue(vm, tasklet);
        if (observeReleaseSource && stack) {
            record('r', stack, depth, nullptr, nullptr, nullptr, identity);
        }
    }

    void AcceptReturn(Tasklet* tasklet, void* instruction)
    {
        MarkTasklet('T', tasklet);
        originalAcceptReturn(tasklet, instruction);
    }

    bool TryAcquireReturnOwner(Object* owner, Stack* stack)
    {
        const bool acquired = originalTryAcquire(owner, stack);
        if (isCapturing() && stack && stack->frames) {
            record(acquired ? 'A' : 'L', stack->stackID, stack->frames - 1, nullptr, nullptr, nullptr, LockToken(owner));
            ObserveAttempt(owner, *stack, acquired);
        }
        return acquired;
    }
}

void ResumeTrace::BeginTrial(bool enabled)
{
    const std::lock_guard guard(waitLock);
    waiters = {};
    entryWaiters = {};
    latestRetry = {};
    retryFirst = enabled;
    entryEnabled.store(enabled, std::memory_order_relaxed);
    entryOverrun.store(false, std::memory_order_relaxed);
}

std::uint64_t ResumeTrace::TaskletStartedAt(const RE::BSScript::Stack* stack)
{
    return activeTasklet.first == stack ? activeTasklet.second : 0;
}

bool ResumeTrace::Install(IsCapturing* capturing, Record* recorder, bool compare, bool compareLateEntry, bool observe,
    bool observeNativeStages, bool releaseSource)
{
    const auto marshalCall = REL::ID(104651).address() + 0x1FF;
    const auto enqueueCall = REL::ID(104854).address() + 0x290;
    const auto acquireCall = REL::ID(104854).address() + 0x190;
    // VMProcess accepts a returned value here regardless of which game job dispatches it.
    const auto resumeCall = REL::ID(105176).address() + 0x81;
    const auto drainCall = REL::ID(104767).address() + 0xFF;
    const auto retryCall = REL::ID(104767).address() + 0x10E;
    const auto taskletProcess = REL::ID(105176).address();
    const auto entryAcquire = REL::ID(104853).address() + 0x2B1;
    constexpr std::array<std::uint8_t, 6> marshalExpected{ 0xFF, 0x90, 0xB0, 0, 0, 0 };
    constexpr std::array<std::uint8_t, 5> enqueueExpected{ 0xE8, 0xAB, 0xFE, 0x00, 0x00 };
    constexpr std::array<std::uint8_t, 5> acquireExpected{ 0xE8, 0x6B, 0x43, 0xFD, 0xFF };
    constexpr std::array<std::uint8_t, 5> resumeExpected{ 0xE8, 0x2A, 0x14, 0x00, 0x00 };
    constexpr std::array<std::uint8_t, 5> drainExpected{ 0xE8, 0x0C, 0xB3, 0x00, 0x00 };
    constexpr std::array<std::uint8_t, 5> retryExpected{ 0xE8, 0x7D, 0xB5, 0x00, 0x00 };
    constexpr std::array<std::uint8_t, 5> entryExpected{ 0xE8, 0x1A, 0x4D, 0xFD, 0xFF };
    constexpr std::array<std::uint8_t, 5> taskletExpected{ 0x40, 0x55, 0x53, 0x56, 0x57 };
    if (!REL::verify_code(marshalCall, marshalExpected.data(), marshalExpected.size()) ||
        !REL::verify_code(enqueueCall, enqueueExpected.data(), enqueueExpected.size()) ||
        !REL::verify_code(acquireCall, acquireExpected.data(), acquireExpected.size()) ||
        !REL::verify_code(resumeCall, resumeExpected.data(), resumeExpected.size()) ||
        (compare && !REL::verify_code(drainCall, drainExpected.data(), drainExpected.size())) ||
        ((compare || compareLateEntry || observe || observeNativeStages) && !REL::verify_code(retryCall, retryExpected.data(), retryExpected.size())) ||
        ((compareLateEntry || observeNativeStages) && !REL::verify_code(taskletProcess, taskletExpected.data(), taskletExpected.size())) ||
        (compareLateEntry && (!REL::verify_code(entryAcquire, entryExpected.data(), entryExpected.size()) ||
            !QueueHeadroom::InstallLimiter()))) {
        return false;
    }

    isCapturing = capturing;
    record = recorder;
    compareRetry = compare;
    compareEntry = compareLateEntry;
    observeRetry = observe;
    observeReleaseSource = releaseSource;
    originalEnqueue = REL::ID(105111).address();
    originalAcceptReturn = REL::ID(105181).address();
    originalTryAcquire = REL::ID(104257).address();
    if (!InstallLockTransitions()) {
        return false;
    }
    auto& trampoline = SKSE::GetTrampoline();
    const auto entry = reinterpret_cast<std::uintptr_t>(Marshal);
    auto* slot = trampoline.allocate(sizeof(entry));
    std::memcpy(slot, &entry, sizeof(entry));
    const auto delta = reinterpret_cast<std::intptr_t>(slot) - static_cast<std::intptr_t>(marshalCall + 6);
    if (delta < std::numeric_limits<std::int32_t>::min() || delta > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    auto patch = marshalExpected;
    patch[1] = 0x15;
    const auto displacement = static_cast<std::int32_t>(delta);
    std::memcpy(patch.data() + 2, &displacement, sizeof(displacement));
    if (!REL::safe_write(marshalCall, patch.data(), 6, marshalExpected)) {
        return false;
    }
    trampoline.write_call<5>(enqueueCall, Enqueue);
    trampoline.write_call<5>(resumeCall, AcceptReturn);
    trampoline.write_call<5>(acquireCall, TryAcquireReturnOwner);
    if (compareRetry || compareEntry || observeRetry || observeNativeStages) {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        frameThreshold = frequency.QuadPart / 50;
        slowFrameThreshold = frequency.QuadPart / 20;
        originalRetry = trampoline.write_call<5>(retryCall, Retry);
    }
    if (compareRetry) {
        originalDrain = trampoline.write_call<5>(drainCall, Drain);
        SKSE::log::info("Installed ordered return retry experiment before native message drain");
    }
    if (compareEntry) {
        entryLimitTicks = frameThreshold / 40;  // Stop further retries this trial after 0.50 ms.
        originalEnter = REL::ID(104853).address();
        std::array<std::uint8_t, 22> entryStub{
            0x4D, 0x8B, 0xC4,               // mov r8, r12 (pointer to the stock query)
            0x4C, 0x8D, 0x4C, 0x24, 0x38    // lea r9, [rsp + 0x38] (name, with return address)
        };
        SetJump(entryStub, 8, reinterpret_cast<std::uintptr_t>(TryAcquireEntryOwner));
        auto* entryCode = trampoline.allocate(entryStub.size());
        std::memcpy(entryCode, entryStub.data(), entryStub.size());
        trampoline.write_call<5>(entryAcquire, reinterpret_cast<std::uintptr_t>(entryCode));
        SKSE::log::info("Installed same-frame ordered entry retry experiment after VMProcess with Display Tweaks reserve observation");
    }
    if (compareEntry || observeNativeStages) {
        std::array<std::uint8_t, 19> taskletStub{};
        std::memcpy(taskletStub.data(), taskletExpected.data(), taskletExpected.size());
        SetJump(taskletStub, taskletExpected.size(), taskletProcess + taskletExpected.size());
        auto* taskletCode = trampoline.allocate(taskletStub.size());
        std::memcpy(taskletCode, taskletStub.data(), taskletStub.size());
        originalTaskletProcess = reinterpret_cast<std::uintptr_t>(taskletCode);
        trampoline.write_branch<5>(taskletProcess, compareEntry ? ProcessTasklet : ObserveTasklet);
    }
    SKSE::log::info("Installed native dispatch, return lock, enqueue, and tasklet resume trace with ownership transitions");
    return true;
}
