#include "Benchmark.h"
#include "BudgetPolicy.h"
#include "IniValues.h"
#include "LogHistory.h"
#include "MenuWatch.h"
#include "NativePacing.h"
#include "VMCatchUp.h"
#ifdef APB_REAL_TELEMETRY
#include "RealBenchmark.h"
#endif

#include <Windows.h>
#include <cwchar>

namespace
{
    using Clock = Benchmark::Clock;
    using RunTasks = void(void*, void*, void*, void*, float);

    constexpr auto runtime = SKSE::RUNTIME_SSE_1_6_1170;
    constexpr auto pluginName = "PapyrusCatchUp";
    constexpr auto iniName = L"PapyrusCatchUp.ini";
    // Logs of earlier game sessions kept beside the current one (LogHistory.h). A log is a few
    // kilobytes, and on a day of many short launches ten of them covered only a few hours.
    constexpr std::size_t keptLogs = 50;
    const std::filesystem::path pluginDirectory = L"Data/SKSE/Plugins";

    REL::Relocation<RunTasks*> originalRunTasks;
    const float* vanillaBudgetMS{};
    const float* engineBudgetScale{};
    const std::uint8_t* originalBudgetInstruction{};
    BudgetPolicy policy;
    Benchmark benchmark;
    std::mutex updateLock;
    Clock::time_point previousUpdate{};
    std::atomic_bool gameplayReady{};
    std::atomic_uint session{};
    unsigned previousSession{};
    bool enableBenchmark{};
    bool enableCorrection{};
    bool enableNativePacing{};
    bool enableCatchUp{};
    // [VMCatchUp] SkipWaitingCalls with Mode=accelerate: the experimental skip rule.
    bool skipWaitingCalls{};
    VMCatchUp::Settings catchUpSettings{};
    bool hooksInstalled{};
    bool budgetCorrectionInstalled{};
    bool nativePacingInstalled{};
    Clock::time_point previousNativeUpdate{};
    bool budgetHookConflict{};

    // Skyrim 1.6.1170 loads the INI float, then scales it to engine budget units.
    constexpr std::array<std::uint8_t, 16> budgetLoad{
        0xF3, 0x0F, 0x10, 0x35, 0xF8, 0x88, 0x65, 0x01,
        0xF3, 0x0F, 0x59, 0x35, 0x5C, 0x38, 0xDB, 0x00
    };
    constexpr std::array<std::uint8_t, 5> taskCall{ 0xE8, 0xD1, 0xF5, 0x00, 0x00 };

    // Reads one INI value without a trailing ';' or '#' comment, which the Win32 reader keeps.
    std::optional<std::wstring> ReadValue(const wchar_t* name, const wchar_t* section, const wchar_t* key)
    {
        const auto path = std::filesystem::absolute(pluginDirectory / name);
        std::array<wchar_t, 128> buffer{};
        if (GetPrivateProfileStringW(section, key, nullptr, buffer.data(),
                static_cast<DWORD>(buffer.size()), path.c_str()) == 0) {
            return std::nullopt;
        }
        std::wstring value{ buffer.data() };
        if (const auto comment = value.find_first_of(L";#"); comment != std::wstring::npos) {
            value.erase(comment);
        }
        const auto last = value.find_last_not_of(L" \t");
        value.erase(last == std::wstring::npos ? 0 : last + 1);
        if (value.empty()) {
            return std::nullopt;
        }
        return value;
    }

    // Another plugin's switch, read for the coexistence check: an ambiguous value counts as enabled,
    // so this plugin stays out of that plugin's way.
    std::optional<bool> ReadFlag(const wchar_t* name, const wchar_t* section, const wchar_t* key)
    {
        const auto value = ReadValue(name, section, key);
        if (!value) {
            return std::nullopt;
        }
        return IniValues::ParseSwitch(*value).value_or(true);
    }

    // One of this plugin's own switches. Only a clear "on" value turns a feature on; a missing key
    // or anything unreadable leaves it off and is reported, so a typo cannot install a hook.
    bool ReadSwitch(const wchar_t* section, const wchar_t* key)
    {
        const auto value = ReadValue(iniName, section, key);
        if (!value) {
            return false;
        }
        if (const auto parsed = IniValues::ParseSwitch(*value)) {
            return *parsed;
        }
        SKSE::log::warn("Unknown INI value [{}] {} = '{}'; the feature stays off (use true or false)",
            SKSE::stl::utf16_to_utf8(section).value_or("?"), SKSE::stl::utf16_to_utf8(key).value_or("?"),
            SKSE::stl::utf16_to_utf8(*value).value_or("?"));
        return false;
    }

    std::optional<double> ReadNumber(const wchar_t* section, const wchar_t* key)
    {
        const auto value = ReadValue(iniName, section, key);
        if (!value) {
            return std::nullopt;
        }
        const auto number = IniValues::ParseNumber(*value);
        if (!number) {
            SKSE::log::warn("Ignoring non-numeric INI value [{}] {}; using the default",
                SKSE::stl::utf16_to_utf8(section).value_or("?"), SKSE::stl::utf16_to_utf8(key).value_or("?"));
        }
        return number;
    }

    // [VMCatchUp] Mode: off (default), carried, or accelerate. Anything else keeps it off.
    std::optional<VMCatchUp::Mode> ReadCatchUpMode()
    {
        if (ReadValue(iniName, L"VMCatchUp", L"Enabled")) {
            SKSE::log::warn("[VMCatchUp] Enabled is no longer read; use Mode=off, carried or accelerate");
        }
        const auto value = ReadValue(iniName, L"VMCatchUp", L"Mode");
        if (!value || _wcsicmp(value->c_str(), L"off") == 0) {
            return std::nullopt;
        }
        if (_wcsicmp(value->c_str(), L"carried") == 0) {
            return VMCatchUp::Mode::kCarried;
        }
        if (_wcsicmp(value->c_str(), L"accelerate") == 0) {
            return VMCatchUp::Mode::kAccelerate;
        }
        SKSE::log::warn("Unknown [VMCatchUp] Mode '{}'; VM catch-up stays off",
            SKSE::stl::utf16_to_utf8(*value).value_or("?"));
        return std::nullopt;
    }

    VMCatchUp::Settings ReadCatchUpSettings(VMCatchUp::Mode mode)
    {
        VMCatchUp::Settings requested{};
        requested.mode = mode;
        if (const auto value = ReadNumber(L"VMCatchUp", L"EngageBelowFPS")) {
            requested.engageHz = *value;
        }
        if (const auto value = ReadNumber(L"VMCatchUp", L"TargetFPS")) {
            requested.targetHz = *value;
        }
        if (const auto value = ReadNumber(L"VMCatchUp", L"MaxExtraCycles")) {
            // Non-integers and out-of-range counts fall back to the default with a warning below.
            requested.maxPasses = *value >= 1.0 && *value <= 1000.0 && std::floor(*value) == *value ?
                static_cast<unsigned>(*value) : 0;
        }
        if (const auto value = ReadNumber(L"VMCatchUp", L"MaxExtraMS")) {
            requested.maxExtraMS = *value;
        }
        const auto sanitized = VMCatchUp::Sanitize(requested);
        if (sanitized.engageHz != requested.engageHz || sanitized.targetHz != requested.targetHz ||
            sanitized.maxPasses != requested.maxPasses || sanitized.maxExtraMS != requested.maxExtraMS) {
            SKSE::log::warn("VMCatchUp INI value out of range; using EngageBelowFPS={:.0f}, TargetFPS={:.0f}, MaxExtraCycles={}, MaxExtraMS={:.2f}",
                sanitized.engageHz, sanitized.targetHz, sanitized.maxPasses, sanitized.maxExtraMS);
        }
        if (sanitized.engageHz > sanitized.targetHz) {
            SKSE::log::warn("VMCatchUp EngageBelowFPS={:.0f} is above TargetFPS={:.0f}; extra cycles start below {:.0f} FPS",
                sanitized.engageHz, sanitized.targetHz, sanitized.targetHz);
        }
        return sanitized;
    }

    bool GameplayGate()
    {
        return gameplayReady.load() && !MenuWatch::Blocked();
    }

    bool DisplayTweaksOwnsBudget()
    {
        if (!GetModuleHandleW(L"SSEDisplayTweaks.dll")) {
            return false;
        }
        const auto base = ReadFlag(L"SSEDisplayTweaks.ini", L"Papyrus", L"DynamicUpdateBudget");
        const auto custom = ReadFlag(L"SSEDisplayTweaks_Custom.ini", L"Papyrus", L"DynamicUpdateBudget");
        return custom.value_or(base.value_or(false));
    }

    class MenuObserver final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
        // Runs on the thread that sent the event, which is not always the same one.
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            // Cell attach and load scripts burst right after a loading screen, as after a save load.
            if (event && !event->opening && event->menuName == RE::LoadingMenu::MENU_NAME) {
                VMCatchUp::NotifyGameLoaded();
            }
            MenuWatch::OnEvent(event);
            if (MenuWatch::Blocked()) {
                // Tasklets on other threads stop taking tickets before the next VM update.
                NativePacing::Pause();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    MenuObserver menuObserver;

    void ResetTiming()
    {
        previousUpdate = {};
        policy.Reset();
        if (enableBenchmark) {
            benchmark.Interrupt();
        }
    }

    float GetBudget(float originalBudget)
    {
        // Game VM updates can move between worker threads.
        const std::lock_guard lock(updateLock);
        if (budgetHookConflict) {
            return originalBudget;
        }
        if (std::memcmp(originalBudgetInstruction, budgetLoad.data(), budgetLoad.size()) != 0) {
            budgetHookConflict = true;
            ResetTiming();
            SKSE::log::warn("Game budget load was patched by another plugin; compensation and benchmark disabled");
            return originalBudget;
        }

        const auto currentSession = session.load();
        if (currentSession != previousSession) {
            previousSession = currentSession;
            ResetTiming();
        }
        const float baseMS = *vanillaBudgetMS;
        if (!GameplayGate() ||
            !std::isfinite(originalBudget) || originalBudget <= 0.0f ||
            originalBudget != baseMS * *engineBudgetScale) {
            ResetTiming();
            return originalBudget;
        }

        const auto now = Clock::now();
        const auto last = std::exchange(previousUpdate, now);
        const double frameMS = last == Clock::time_point{} ? 0.0 :
            std::chrono::duration<double, std::milli>(now - last).count();

        // Benchmark measures end-to-end script latency with the same hook loaded in both modes.
        const bool active = enableCorrection && (!enableBenchmark || benchmark.Enabled(now));
        const float multiplier = static_cast<float>(policy.Update(frameMS, active));
        if (enableBenchmark) {
#ifdef APB_STRESS_TELEMETRY
            benchmark.ObservePolicy(policy.Observe(frameMS, active));
#endif
            benchmark.Sample(now, frameMS, baseMS, baseMS * multiplier);
        }
        return originalBudget * multiplier;
    }

    void RunTasksWithBudget(void* vm, void* queue, void* otherQueue, void* lock, float originalBudget)
    {
        if (nativePacingInstalled) {
            const std::lock_guard guard(updateLock);
            if (GameplayGate()) {
                const auto now = Clock::now();
                const auto last = std::exchange(previousNativeUpdate, now);
                const auto interval = last == Clock::time_point{} ? Clock::duration{} : now - last;
                bool promote = interval > std::chrono::milliseconds(20) && interval < std::chrono::milliseconds(100);
#ifdef APB_REAL_TELEMETRY
                if (enableBenchmark) {
                    promote = RealBenchmark::Update(now, interval, originalBudget) && promote;
                }
#endif
                NativePacing::BeginUpdate(promote);
            } else {
                previousNativeUpdate = {};
                NativePacing::Pause();
#ifdef APB_REAL_TELEMETRY
                if (enableBenchmark) {
                    RealBenchmark::Interrupt();
                }
#endif
            }
        }
        originalRunTasks(vm, queue, otherQueue, lock,
            budgetCorrectionInstalled ? GetBudget(originalBudget) : originalBudget);
    }

    void InstallBudgetHooks()
    {
        bool installBudget = enableCorrection;
        if (!installBudget) {
            SKSE::log::info("Compensation disabled in plugin INI");
        } else if (DisplayTweaksOwnsBudget()) {
            SKSE::log::info("SSE Display Tweaks DynamicUpdateBudget is enabled; leaving its budget in control");
            installBudget = false;
        }
        if (!installBudget && !enableNativePacing) {
            return;
        }
        const REL::Relocation<std::uintptr_t> runGame{ REL::ID(53928) };
        const auto loadAddress = runGame.address() + 0x90;
        const auto callAddress = runGame.address() + 0xBA;
        if (!REL::verify_code(callAddress, taskCall.data(), taskCall.size())) {
            SKSE::log::warn("Gameplay VM call has been patched or is unsupported; leaving the VM unchanged");
            return;
        }
        if (installBudget && !REL::verify_code(loadAddress, budgetLoad.data(), budgetLoad.size())) {
            SKSE::log::warn("Game budget load differs from 1.6.1170; leaving its budget in control");
            installBudget = false;
        }
        nativePacingInstalled = enableNativePacing && NativePacing::Install();
        if (!installBudget && !nativePacingInstalled) {
            return;
        }
        if (installBudget) {
            // Decode both verified RIP-relative operands. The Setting is read-only here.
            std::int32_t budgetOffset{};
            std::int32_t scaleOffset{};
            std::memcpy(&budgetOffset, budgetLoad.data() + 4, sizeof(budgetOffset));
            std::memcpy(&scaleOffset, budgetLoad.data() + 12, sizeof(scaleOffset));
            vanillaBudgetMS = reinterpret_cast<const float*>(loadAddress + 8 + budgetOffset);
            engineBudgetScale = reinterpret_cast<const float*>(loadAddress + 16 + scaleOffset);
            originalBudgetInstruction = reinterpret_cast<const std::uint8_t*>(loadAddress);
            budgetCorrectionInstalled = true;
        }
        originalRunTasks = SKSE::GetTrampoline().write_call<5>(callAddress, RunTasksWithBudget);
        SKSE::log::info("Installed gameplay VM hook; budget correction={}, native pacing={}, benchmark={}",
            budgetCorrectionInstalled, nativePacingInstalled, enableBenchmark);
    }

    void Install()
    {
        InstallBudgetHooks();
        if (!enableCatchUp) {
            SKSE::log::info("VM catch-up is off ([VMCatchUp] Mode=off)");
        } else {
            if (skipWaitingCalls) {
                VMCatchUp::SetSkipBlocked(true);
            }
            VMCatchUp::Install(catchUpSettings, GameplayGate);
        }
        hooksInstalled = originalRunTasks.address() != 0 || VMCatchUp::Installed();
    }

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        switch (message->type) {
        case SKSE::MessagingInterface::kPostPostLoad:
            Install();
            break;
        case SKSE::MessagingInterface::kInputLoaded:
            if (hooksInstalled) {
                if (auto* ui = RE::UI::GetSingleton()) {
                    ui->AddEventSink(&menuObserver);
                }
            }
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            gameplayReady.store(false);
            NativePacing::Pause();
            ++session;
            break;
        case SKSE::MessagingInterface::kNewGame:
        case SKSE::MessagingInterface::kPostLoadGame: {
            if (hooksInstalled) {
                const bool ready = message->type == SKSE::MessagingInterface::kNewGame || message->data != nullptr;
                MenuWatch::Sync();
                NativePacing::Pause();
                if (ready) {
                    VMCatchUp::NotifyGameLoaded();
                }
                gameplayReady.store(ready);
                if (enableBenchmark) {
                    SKSE::log::info("Benchmark gameplay ready={}", ready);
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

SKSE_PLUGIN_VERSION = [] {
    SKSE::PluginVersionData info{};
    info.PluginName(pluginName);
    info.PluginVersion({ 0, 3, 0, 0 });
    info.CompatibleVersions({ runtime });
    info.UsesUpdatedStructs();
    return info;
}();

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    if (skse->RuntimeVersion() != runtime) {
        return false;
    }
    // Before the log is opened: the previous session's log is moved aside instead of being overwritten.
    const auto logDirectory = SKSE::log::log_directory();
    const auto history = logDirectory ? LogHistory::Keep(*logDirectory, pluginName, keptLogs) : LogHistory::Result{};
    SKSE::Init(skse, { .logName = pluginName, .trampoline = true, .trampolineSize = 256 });
    if (!history.kept.empty()) {
        SKSE::log::info("The previous session's log was kept as {}; {} older log(s) removed, up to {} are kept",
            history.kept.filename().string(), history.removed, keptLogs);
    }
    if (!history.problem.empty()) {
        SKSE::log::warn("Earlier logs: {}", history.problem);
    }
    // Off unless asked for: the legacy budget correction showed no measurable effect.
    enableCorrection = ReadSwitch(L"General", L"Enabled");
    enableBenchmark = ReadSwitch(L"General", L"Benchmark");
    if (enableBenchmark && !enableCorrection) {
        // The benchmark alternates the budget correction on and off; without it there is nothing to compare.
        SKSE::log::warn("[General] Benchmark=true needs [General] Enabled=true; the benchmark will not run");
    }
    enableNativePacing = ReadSwitch(L"NativePacing", L"Enabled");
    const auto mode = ReadCatchUpMode();
    if (mode) {
        enableCatchUp = true;
        catchUpSettings = ReadCatchUpSettings(*mode);
    }
    // Experimental and off unless asked for, like every switch of this plugin. The rule is defined
    // on accelerate's judgement of the calls, so it is refused, and said so, in any other mode.
    if (ReadSwitch(L"VMCatchUp", L"SkipWaitingCalls")) {
        if (mode == VMCatchUp::Mode::kAccelerate) {
            skipWaitingCalls = true;
        } else {
            SKSE::log::warn("[VMCatchUp] SkipWaitingCalls=true needs Mode=accelerate; it is ignored");
        }
    }
    return SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
}
