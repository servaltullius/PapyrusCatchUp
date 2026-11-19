#include "QueueHeadroom.h"

#include <Windows.h>
#include <intrin.h>

namespace
{
    using VM = RE::BSScript::Internal::VirtualMachine;
    using Message = RE::BSScript::Internal::FunctionMessage;
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    using Drain = void(VM*, float, std::uint32_t*, std::uint32_t*);
    using Pop = bool(VM*, Message*);

    REL::Relocation<Drain*> originalDrain;
    REL::Relocation<Pop*> originalPop;
    decltype(&Sleep) originalSleep{};
    const void* limiterReturn{};
    std::atomic<Clock::duration::rep> firstYield{};
    std::atomic<Clock::duration::rep> lastYield{};
    std::atomic<Clock::duration::rep> gameplayUntil{};
    std::atomic_bool measuring{};
    std::atomic_bool enabled{};
    std::atomic_uint updates{}, samples{}, withHeadroom{}, extended{}, overtimePops{}, popped{}, totalExtraUS{};
    std::atomic_uint yieldBursts{}, drainThread{}, observedGameThread{};
    thread_local Clock::time_point previousDrain{};
    thread_local Clock::time_point drainStart{};
    thread_local float baseMS{};
    thread_local bool recordPops{};

    void WINAPI ObserveYield(DWORD milliseconds)
    {
        if (milliseconds == 0 && _ReturnAddress() == limiterReturn) {
            const auto now = Clock::now().time_since_epoch();
            const Clock::duration last{ lastYield.load(std::memory_order_relaxed) };
            if (now - last > 2ms) {
                firstYield.store(now.count(), std::memory_order_relaxed);
                if (measuring.load(std::memory_order_relaxed)) {
                    ++yieldBursts;
                }
            }
            lastYield.store(now.count(), std::memory_order_release);
        }
        originalSleep(milliseconds);
    }

    unsigned ExtraMicroseconds(Clock::time_point now, Clock::duration interval)
    {
        if (interval <= 20ms || interval >= 50ms) {
            return 0;
        }
        const Clock::duration last{ lastYield.load(std::memory_order_acquire) };
        const Clock::duration first{ firstYield.load(std::memory_order_relaxed) };
        const auto age = now.time_since_epoch() - last;
        if (age < 0ms || age > 25ms || last.count() != lastYield.load(std::memory_order_relaxed)) {
            return 0;
        }
        const auto available = std::chrono::duration_cast<std::chrono::microseconds>(last - first - 1500us);
        return available >= 250us ? static_cast<unsigned>(std::min(available, 750us).count()) : 0;
    }

    bool ObservePop(VM* vm, Message* message)
    {
        const bool pastBase = recordPops &&
            std::chrono::duration<float, std::milli>(Clock::now() - drainStart).count() > baseMS;
        const bool result = originalPop(vm, message);
        if (recordPops && result) {
            ++popped;
            if (pastBase) {
                ++overtimePops;
            }
        }
        return result;
    }

    void DrainWithHeadroom(VM* vm, float budgetMS, std::uint32_t* running, std::uint32_t* pending)
    {
        const auto now = Clock::now();
        const auto interval = now - std::exchange(previousDrain, now);
        unsigned extraUS = 0;
        const bool active = measuring.load(std::memory_order_relaxed);
        if (active) {
            ++updates;
            drainThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        }
        // The scheduler keeps its own caller. UI state is published by the game-thread probe.
        if (active && std::isfinite(budgetMS) && budgetMS > 0.0f &&
            now.time_since_epoch().count() < gameplayUntil.load(std::memory_order_relaxed)) {
            ++samples;
            const auto reserve = ExtraMicroseconds(now, interval);
            if (reserve) {
                ++withHeadroom;
                if (enabled.load(std::memory_order_relaxed)) {
                    extraUS = reserve;
                    ++extended;
                    totalExtraUS.fetch_add(extraUS, std::memory_order_relaxed);
                }
            }
            drainStart = Clock::now();
            baseMS = budgetMS;
            recordPops = true;
        }
        // Keep the original queue, pop order, deadline checks, and caller thread.
        originalDrain(vm, budgetMS + static_cast<float>(extraUS) / 1000.0f, running, pending);
        recordPops = false;
    }
}

bool QueueHeadroom::HasRecentReserve()
{
    return ExtraMicroseconds(Clock::now(), 24ms) > 0;
}

bool QueueHeadroom::InstallLimiter()
{
    const auto display = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"SSEDisplayTweaks.dll"));
    if (!display) {
        return false;
    }
    // Display Tweaks 0.5.16: zero-duration yield inside FramerateLimiter::WaitBusy.
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(display);
    const auto* image = reinterpret_cast<const IMAGE_NT_HEADERS64*>(display + dos->e_lfanew);
    if (image->FileHeader.TimeDateStamp != 1703410653 || image->OptionalHeader.SizeOfImage <= 0x44048) {
        return false;
    }
    const auto sleepSlot = display + 0x44040;
    const auto sleepCall = display + 0x285BE;
    constexpr std::array<std::uint8_t, 12> limiterCode{
        0x7D, 0x0A, 0x33, 0xC9, 0xFF, 0x15, 0x7C, 0xBA, 0x01, 0x00, 0xEB, 0xDA
    };
    if (!REL::verify_code(sleepCall - 4, limiterCode.data(), limiterCode.size())) {
        return false;
    }
    originalSleep = reinterpret_cast<decltype(originalSleep)>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Sleep"));
    limiterReturn = reinterpret_cast<const void*>(sleepCall + 6);
    const auto hook = &ObserveYield;
    return originalSleep && REL::safe_write(sleepSlot, &hook, sizeof(hook), &originalSleep, sizeof(originalSleep));
}

bool QueueHeadroom::Install()
{
    constexpr std::array<std::uint8_t, 5> drainCode{ 0xE8, 0x0C, 0xB3, 0x00, 0x00 };
    constexpr std::array<std::uint8_t, 5> popCode{ 0xE8, 0x7C, 0x21, 0x00, 0x00 };
    const auto drainCall = REL::ID(104767).address() + 0xFF;
    const auto popCall = REL::ID(104857).address() + 0x1BF;
    if (!REL::verify_code(drainCall, drainCode.data(), drainCode.size()) ||
        !REL::verify_code(popCall, popCode.data(), popCode.size()) || !InstallLimiter()) {
        return false;
    }
    originalPop = SKSE::GetTrampoline().write_call<5>(popCall, ObservePop);
    originalDrain = SKSE::GetTrampoline().write_call<5>(drainCall, DrainWithHeadroom);
    SKSE::log::info("Installed queue headroom experiment, extra limit 750 us, reserve 1500 us");
    return true;
}

void QueueHeadroom::ObserveGameplay(bool ready)
{
    if (ready) {
        observedGameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    }
    const auto until = ready ? Clock::now() + 500ms : Clock::time_point{};
    gameplayUntil.store(until.time_since_epoch().count(), std::memory_order_relaxed);
}

void QueueHeadroom::BeginTrial(bool active)
{
    updates = samples = withHeadroom = extended = overtimePops = popped = totalExtraUS = yieldBursts = 0;
    enabled.store(active);
    measuring.store(true);
}

bool QueueHeadroom::EndTrial()
{
    measuring.store(false);
    const bool wasEnabled = enabled.exchange(false);
    SKSE::log::info("queue headroom samples={} updates={} yield_bursts={} reserve_frames={} extended={} "
        "stock_loop_pops={} past_base_pops={} extra_us={} drain_thread={} game_thread={}",
        samples.load(), updates.load(), yieldBursts.load(), withHeadroom.load(), extended.load(), popped.load(),
        overtimePops.load(), totalExtraUS.load(), drainThread.load(), observedGameThread.load());
    return samples.load() > 0 && (extended.load() > 0) == wasEnabled;
}
