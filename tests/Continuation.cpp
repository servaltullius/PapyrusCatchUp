#include "Continuation.h"

#include "RE/C/CodeTasklet.h"
#include "RE/N/NativeFunctionBase.h"
#include "RE/O/Object.h"
#include "RE/S/StackFrame.h"
#include "RE/S/State.h"

#include <Windows.h>
#include <limits>

#undef GetObject

namespace
{
    using VM = RE::BSScript::Internal::VirtualMachine;
    using Tasklet = RE::BSScript::Internal::CodeTasklet;
    using Stack = RE::BSScript::Stack;
    using Function = RE::BSScript::IFunction;
    using Query = RE::BSTSmartPointer<RE::BSScript::Internal::IFuncCallQuery>;

    enum class Read { none, worldspace, at };

    struct Ready
    {
        Tasklet* tasklet{};
        RE::VMStackID stack{};
        Read read{};
    };

    struct Phase
    {
        std::array<Ready, 16> ready{};
        std::size_t count{};
        RE::VMStackID lastStack{};
        Read lastRead{};
        std::uint32_t frame{};
        std::uint64_t deadline{};
    };

    struct Slice
    {
        RE::VMStackID stack{};
        Read next{};
        std::uint32_t frame{};
        std::uint64_t deadline{};
    };

    REL::Relocation<void(*)(VM*, float, std::uint32_t*, std::uint32_t*)> originalDrain;
    REL::Relocation<void(*)(VM*, float, std::uint32_t*)> originalRetry;
    REL::Relocation<void(*)(Tasklet*)> process;
    REL::Relocation<std::uint32_t(*)(VM*, Stack*, const Query&, bool)> originalEnter;
    ResumeTrace::Record* record{};
    thread_local Phase* currentPhase{};
    thread_local Slice currentSlice;
    std::atomic_bool armed{}, enabled{}, overrun{};
    std::atomic_uint32_t usedFrame{};
    std::atomic_uint64_t previousDrain{}, lastInterval{};
    std::uint64_t minimumFrame{}, maximumFrame{}, sliceBudget{}, overrunLimit{};
    double ticksPerMS{};
    std::uint64_t minimumReserve{};
    std::uintptr_t worldVtable{}, atVtable{}, worldCallback{}, atCallback{};
    bool worldspaceOnly{};

    std::uint64_t QPC()
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    std::uint32_t RenderFrame()
    {
        return static_cast<volatile const std::uint32_t&>(RE::BSGraphics::State::GetSingleton()->GetFrameCount());
    }

    Read StockRead(const Function& function)
    {
        std::uintptr_t vtable{}, callback{};
        std::memcpy(&vtable, &function, sizeof(vtable));
        if (vtable != worldVtable && vtable != atVtable) {
            return Read::none;
        }
        std::memcpy(&callback, reinterpret_cast<const std::byte*>(&function) + 0x50, sizeof(callback));
        if (vtable == worldVtable && callback == worldCallback &&
            function.GetObjectTypeName() == "ObjectReference" && function.GetName() == "GetWorldSpace") {
            return Read::worldspace;
        }
        if (vtable == atVtable && callback == atCallback &&
            function.GetObjectTypeName() == "FormList" && function.GetName() == "GetAt") {
            return Read::at;
        }
        return Read::none;
    }

    bool DynDOLODRoot(const RE::BSScript::StackFrame* frame)
    {
        const auto* function = frame ? frame->owningFunction.get() : nullptr;
        return function && !frame->previousFrame && function->GetObjectTypeName() == "SHESON_DynDOLOD_ObjectEnabler" &&
            (function->GetName() == "OnCellAttach" || function->GetName() == "OnCellDetach");
    }

    RE::VMHandle BoundHandle(const RE::BSScript::Variable& variable)
    {
        if (variable.IsObject()) {
            if (const auto object = variable.GetObject()) {
                return object->GetHandle();
            }
        }
        return 0;
    }

    bool Callable(const Function* function, const Stack* stack)
    {
        if (currentSlice.next != Read::none) {
            const auto expected = std::exchange(currentSlice.next, Read::none);
            if (stack && stack->stackID == currentSlice.stack && StockRead(*function) == expected &&
                QPC() < currentSlice.deadline &&
                RenderFrame() == currentSlice.frame) {
                const auto& type = function->GetObjectTypeName();
                const auto& name = function->GetName();
                record('i', currentSlice.stack, 1, &type, nullptr, &name, currentSlice.frame);
                return true;
            }
        }
        return function->CanBeCalledFromTasklets();
    }

    void ContinueHead(VM& vm, const Phase& phase)
    {
        const auto start = QPC();
        const auto frame = phase.frame;
        if (!phase.count || usedFrame.load(std::memory_order_relaxed) == frame) {
            return;
        }
        if (RenderFrame() != frame || vm.overstressed || vm.freezeState != VM::FreezeState::kNotFrozen || vm.frozenStacks) {
            record('w', phase.ready[0].stack, 0, nullptr, nullptr, nullptr, 1);
            return;
        }
        const auto deadline = std::min(phase.deadline, start + sliceBudget);
        if (start + minimumReserve >= deadline) {
            record('w', phase.ready[0].stack, 0, nullptr, nullptr, nullptr, 2);
            return;
        }
        RE::BSTSmartPointer<Tasklet> tasklet;
        Ready ready;
        {
            const RE::BSSpinLockGuard guard(vm.funcQueueLock);
            if (vm.vmTasks.empty() || vm.vmTasks.size() > 16) {
                record('m', phase.ready[0].stack, 0, nullptr, nullptr, nullptr, vm.vmTasks.size());
                return;
            }
            auto* head = vm.vmTasks.front();
            const auto candidates = std::span(phase.ready).first(phase.count);
            const auto found = std::ranges::find(candidates, head, &Ready::tasklet);
            if (found == candidates.end() || head->vm != &vm || !head->stack || head->stack->stackID != found->stack ||
                head->stack->owningTasklet.get() != head || head->stack->frames != 1 ||
                head->resumeReason != Tasklet::ResumeReason::kFunctionReturn || !DynDOLODRoot(head->stack->top) ||
                std::ranges::count(vm.vmTasks, head) != 1) {
                record('m', phase.ready[0].stack, 0, nullptr, nullptr, nullptr, vm.vmTasks.size());
                return;
            }
            ready = *found;
            if (!enabled.load(std::memory_order_relaxed)) {
                record('H', ready.stack, 0, nullptr, nullptr, nullptr, frame);
                return;
            }
            if (overrun.load(std::memory_order_relaxed) || !armed.load(std::memory_order_relaxed) ||
                RenderFrame() != frame || QPC() + minimumReserve >= deadline) {
                return;
            }
            if (usedFrame.exchange(frame, std::memory_order_relaxed) == frame) {
                return;
            }
            tasklet.reset(head);
            vm.vmTasks.erase(vm.vmTasks.begin());
        }

        record('h', ready.stack, 0, nullptr, nullptr, nullptr, frame);
        currentSlice = { ready.stack, ready.read == Read::worldspace ? Read::at : Read::worldspace, frame, deadline };
        process(tasklet.get());
        currentSlice = {};
        const auto spent = QPC() - start;
        record('k', ready.stack, 0, nullptr, nullptr, nullptr, spent);
        if (spent > overrunLimit) {
            overrun.store(true, std::memory_order_relaxed);
        }
    }

    std::uint32_t Enter(VM* vm, Stack* stack, const Query& query, bool callingFromTasklets)
    {
        auto* phase = currentPhase;
        if (phase) {
            phase->count = 0;
            phase->lastRead = Read::none;
        }
        const auto result = originalEnter(vm, stack, query, callingFromTasklets);
        if (phase && phase->count) {
            currentPhase = nullptr;
            ContinueHead(*vm, *phase);
            currentPhase = phase;
        }
        return result;
    }

    template <class Callback>
    void NativePhase(float budget, Callback&& callback)
    {
        const auto interval = lastInterval.load(std::memory_order_relaxed);
        if (!armed.load(std::memory_order_relaxed) || interval <= minimumFrame || interval >= maximumFrame ||
            !std::isfinite(budget) || budget <= 0.0f || budget >= 5.0f) {
            callback();
            return;
        }
        Phase phase;
        phase.frame = RenderFrame();
        phase.deadline = QPC() + static_cast<std::uint64_t>(budget * ticksPerMS);
        auto* previous = std::exchange(currentPhase, &phase);
        callback();
        currentPhase = previous;
    }

    void Drain(VM* vm, float budget, std::uint32_t* running, std::uint32_t* pending)
    {
        const auto now = QPC();
        lastInterval.store(now - previousDrain.exchange(now, std::memory_order_relaxed), std::memory_order_relaxed);
        NativePhase(budget, [&] { originalDrain(vm, budget, running, pending); });
    }

    void Retry(VM* vm, float budget, std::uint32_t* pending)
    {
        NativePhase(budget, [&] { originalRetry(vm, budget, pending); });
    }
}

bool Continuation::VerifyWorldSpace()
{
    const auto base = REL::Module::get().base();
    worldVtable = base + 0x192E4F0;
    worldCallback = base + 0xA44BA0;
    constexpr char thunk[] = "\x49\x8B\xC8\xE9\xC8\x8C\x8A\xFF";
    constexpr char body[] =
        "\x48\x83\xEC\x28\x48\x8B\x41\x60\x48\x85\xC0\x75\x0E\x48\x8B\x01\xFF\x90\xB8\x04\x00\x00"
        "\x48\x85\xC0\x74\x06\xF6\x40\x40\x01\x74\x07\x33\xC0\x48\x83\xC4\x28\xC3"
        "\x48\x8B\x80\x28\x01\x00\x00\x48\x83\xC4\x28\xC3";
    return REL::verify_code(worldCallback, thunk, sizeof(thunk) - 1) &&
        REL::verify_code(REL::ID(19816).address(), body, sizeof(body) - 1);
}

bool Continuation::IsStockWorldSpace(const RE::BSScript::IFunction& function)
{
    return StockRead(function) == Read::worldspace;
}

bool Continuation::IsDynDOLODRoot(const RE::BSScript::StackFrame* frame)
{
    return DynDOLODRoot(frame);
}

void Continuation::BeginTrial(bool active)
{
    armed.store(false, std::memory_order_relaxed);
    enabled.store(active, std::memory_order_relaxed);
    overrun.store(false, std::memory_order_relaxed);
}

void Continuation::Arm(bool ready)
{
    armed.store(ready, std::memory_order_relaxed);
}

void Continuation::ObserveNative(const RE::BSScript::NF_util::NativeFunctionBase& function,
    const RE::BSScript::StackFrame& frame, bool succeeded)
{
    if (!currentPhase) {
        return;
    }
    currentPhase->lastStack = 0;
    currentPhase->lastRead = Read::none;
    if (succeeded && frame.parent && frame.parent->frames == 2 && DynDOLODRoot(frame.previousFrame)) {
        const auto read = StockRead(function);
        currentPhase->lastRead = !worldspaceOnly || read == Read::at ? read : Read::none;
        currentPhase->lastStack = frame.parent->stackID;
    }
}

void Continuation::ObserveResult(const RE::BSScript::NF_util::NativeFunctionBase& function,
    const RE::BSScript::StackFrame& frame, const RE::BSScript::Variable& self, const RE::BSScript::Variable& result)
{
    if (worldspaceOnly && frame.parent && frame.parent->frames == 2 && StockRead(function) == Read::worldspace &&
        DynDOLODRoot(frame.previousFrame)) {
        const auto stack = frame.parent->stackID;
        record('y', stack, 1, nullptr, nullptr, nullptr, BoundHandle(self));
        record('Y', stack, 1, nullptr, nullptr, nullptr, BoundHandle(result));
    }
}

void Continuation::ObserveReturn(Tasklet* tasklet)
{
    if (currentPhase && currentPhase->lastRead != Read::none && tasklet && tasklet->stack &&
        tasklet->stack->stackID == currentPhase->lastStack &&
        tasklet->resumeReason == Tasklet::ResumeReason::kFunctionReturn && currentPhase->count < currentPhase->ready.size()) {
        currentPhase->ready[currentPhase->count++] = { tasklet, currentPhase->lastStack, currentPhase->lastRead };
        record('f', currentPhase->lastStack, 0, nullptr, nullptr, nullptr, 0);
        currentPhase->lastRead = Read::none;
    }
}

bool Continuation::Install(ResumeTrace::Record* recorder, bool onlyWorldspace)
{
    const auto base = REL::Module::get().base();
    atVtable = base + 0x1922DF8;
    atCallback = base + 0xA17640;
    const auto atBody = REL::ID(20907).address();
    const auto drainCall = REL::ID(104767).address() + 0xFF;
    const auto retryCall = REL::ID(104767).address() + 0x10E;
    const auto drainEnterCall = REL::ID(104857).address() + 0x18E;
    const auto retryEnterCall = REL::ID(104858).address() + 0x44F;
    const auto eligibility = REL::ID(104853).address() + 0x61D;
    constexpr std::array<std::uint8_t, 5> drainCode{ 0xE8, 0x0C, 0xB3, 0x00, 0x00 };
    constexpr std::array<std::uint8_t, 5> retryCode{ 0xE8, 0x7D, 0xB5, 0x00, 0x00 };
    constexpr std::array<std::uint8_t, 5> drainEnterCode{ 0xE8, 0xBD, 0xEB, 0xFF, 0xFF };
    constexpr std::array<std::uint8_t, 5> retryEnterCode{ 0xE8, 0x7C, 0xE6, 0xFF, 0xFF };
    constexpr std::array<std::uint8_t, 13> eligibilityCode{
        0x48, 0x8B, 0x01, 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x75, 0x13
    };
    constexpr char atThunk[] = "\x41\x8B\xD1\x49\x8B\xC8\xE9\x65\x25\x90\xFF";
    constexpr char atCode[] =
        "\x48\x89\x5C\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xEC\x20\x33\xC0\x8B\xDA\x48\x8B\xF1"
        "\x8B\xF8\x48\x8B\x49\x38\x48\x85\xC9\x74\x17\x8B\x79\x10\x3B\xDF\x73\x10\x48\x8B\x01"
        "\x8B\x0C\x98\xE8\xBD\x65\xEC\xFF\x48\x85\xC0\x75\x0F\x2B\xDF\x3B\x5E\x30\x73\x08"
        "\x48\x8B\x46\x20\x48\x8B\x04\xD8\x48\x8B\x5C\x24\x30\x48\x8B\x74\x24\x38\x48\x83\xC4\x20\x5F\xC3";
    if (!REL::verify_code(drainCall, drainCode.data(), drainCode.size()) ||
        !REL::verify_code(retryCall, retryCode.data(), retryCode.size()) ||
        !REL::verify_code(drainEnterCall, drainEnterCode.data(), drainEnterCode.size()) ||
        !REL::verify_code(retryEnterCall, retryEnterCode.data(), retryEnterCode.size()) ||
        !REL::verify_code(eligibility, eligibilityCode.data(), eligibilityCode.size()) ||
        !VerifyWorldSpace() ||
        !REL::verify_code(atCallback, atThunk, sizeof(atThunk) - 1) ||
        !REL::verify_code(atBody, atCode, sizeof(atCode) - 1)) {
        return false;
    }

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    minimumFrame = frequency.QuadPart / 50;
    maximumFrame = frequency.QuadPart / 35;
    sliceBudget = frequency.QuadPart / 4000;
    overrunLimit = frequency.QuadPart / 2000;
    minimumReserve = frequency.QuadPart / 50000;
    ticksPerMS = static_cast<double>(frequency.QuadPart) / 1000.0;
    process = REL::ID(105176).address();
    originalEnter = REL::ID(104853).address();
    record = recorder;
    worldspaceOnly = onlyWorldspace;

    // Replay the stock eligibility branch after passing the current VM stack (rbx).
    std::array<std::uint8_t, 17> jump{ 0x48, 0x8B, 0xD3, 0xFF, 0x25, 0, 0, 0, 0 };
    const auto target = reinterpret_cast<std::uintptr_t>(Callable);
    std::memcpy(jump.data() + 9, &target, sizeof(target));
    auto& trampoline = SKSE::GetTrampoline();
    auto* stub = trampoline.allocate(jump.size());
    std::memcpy(stub, jump.data(), jump.size());
    const auto delta = reinterpret_cast<std::intptr_t>(stub) - static_cast<std::intptr_t>(eligibility + 5);
    if (delta < std::numeric_limits<std::int32_t>::min() || delta > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    std::array<std::uint8_t, eligibilityCode.size()> patch{
        0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x84, 0xC0, 0x75, 0x13
    };
    const auto relative = static_cast<std::int32_t>(delta);
    std::memcpy(patch.data() + 1, &relative, sizeof(relative));
    if (!REL::safe_write(eligibility, patch.data(), patch.size(), eligibilityCode)) {
        return false;
    }
    originalDrain = trampoline.write_call<5>(drainCall, Drain);
    originalRetry = trampoline.write_call<5>(retryCall, Retry);
    trampoline.write_call<5>(drainEnterCall, Enter);
    trampoline.write_call<5>(retryEnterCall, Enter);
    SKSE::log::info("Installed ordered DynDOLOD read continuation on stock VM native phases");
    return true;
}
