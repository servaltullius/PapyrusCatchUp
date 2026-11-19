#include "Benchmark.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <numeric>

namespace
{
    using Clock = Benchmark::Clock;
    using namespace std::chrono_literals;

    enum class Mode { Off, On, Paced };
    using enum Mode;

    constexpr int jobs = 160;
    constexpr int slices = 48;
    constexpr unsigned pacedLimit = 4;
    constexpr auto probeInterval = 250ms;
    constexpr std::array normalModes{ Off, On, On, Off, On, Off, Off, On };
    constexpr std::array taskletModes{ Off, On, On, Off };
    constexpr std::array reverseModes{ On, Off, Off, On };
    constexpr std::array pacedModes{ Off, On, Paced, Paced, On, Off };
    constexpr std::array reorderedPacedModes{ On, Paced, Off, Off, Paced, On };

    std::string_view ModeName(Mode mode)
    {
        switch (mode) {
        case Off: return "off";
        case On: return "on";
        case Paced: return "paced";
        }
        std::unreachable();
    }

    double Milliseconds(Clock::duration duration)
    {
        return std::chrono::duration<double, std::milli>(duration).count();
    }

    double Percentile(std::vector<double> values, double q)
    {
        if (values.empty()) {
            return 0.0;
        }
        std::ranges::sort(values);
        return values[static_cast<std::size_t>(std::ceil(values.size() * q)) - 1];
    }

    RE::BSTSmartPointer<RE::BSScript::IFunction> FindWorkSlice()
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> type;
        if (!vm || !vm->GetScriptObjectTypeNoLoad("APBStressWork", type) || !type) {
            return {};
        }
        const auto* functions = type->GetGlobalFuncIter();
        if (functions) {
            for (std::uint32_t index = 0; index < type->GetNumGlobalFuncs(); ++index) {
                const auto& function = functions[index].func;
                if (function && function->GetName() == "WorkSlice" && function->GetIsNative()) {
                    return function;
                }
            }
        }
        return {};
    }

    struct Results
    {
        std::mutex lock;
        std::vector<double> jobMS;
        std::vector<double> probeMS;
        Clock::time_point lastJob{};
        unsigned badChecksums{};
    };

    class ResultCallback final : public RE::BSScript::IStackCallbackFunctor
    {
    public:
        ResultCallback(std::shared_ptr<Results> results, int expectedSlices, Clock::time_point started) :
            results_(std::move(results)), expectedSlices_(expectedSlices), started_(started)
        {}

        void operator()(RE::BSScript::Variable value) override
        {
            const auto now = Clock::now();
            const double elapsedMS = Milliseconds(now - started_);
            const std::lock_guard guard(results_->lock);
            if (expectedSlices_ == 0) {
                results_->probeMS.push_back(elapsedMS);
            } else {
                results_->jobMS.push_back(elapsedMS);
                results_->lastJob = std::max(results_->lastJob, now);
                if (!value.IsInt() || value.GetSInt() != expectedSlices_) {
                    ++results_->badChecksums;
                }
            }
        }

        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

    private:
        std::shared_ptr<Results> results_;
        int expectedSlices_;
        Clock::time_point started_;
    };

    bool Dispatch(const std::shared_ptr<Results>& results, int workSlices, Clock::time_point started = Clock::now())
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return false;
        }
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{
            new ResultCallback(results, workSlices, started)
        };
        if (workSlices > 0) {
            return vm->DispatchStaticCall("APBStressWork", "Run",
                RE::MakeFunctionArguments(int{ workSlices }), callback);
        }
        return vm->DispatchStaticCall("Utility", "Wait", RE::MakeFunctionArguments(0.01f), callback);
    }

    struct Experiment
    {
        enum class Stage { Warmup, Settle, Burst, Complete };

        Stage stage{ Stage::Warmup };
        Clock::time_point start{};
        Clock::time_point next{};
        Clock::time_point nextProbe{};
        Clock::time_point burstStart{};
        Clock::time_point submittedAt{};
        std::shared_ptr<Results> result;
        RE::BSTSmartPointer<RE::BSScript::IFunction> workSlice;
        std::span<const Mode> modes{ normalModes };
        std::vector<double> frames;
        std::array<unsigned, 6> policyFrames{};
        BudgetPolicy::State latestPolicy{};
        unsigned trial{};
        unsigned probes{};
        unsigned boostedFrames{};
        unsigned accepted{};
        unsigned releaseUpdates{};
        unsigned peakReleased{};
        unsigned peakInFlight{};
        int pendingAt1s{ -1 };
        int pendingAt2s{ -1 };
        double budgetSum{};
        bool preflightStarted{};
        bool taskletAB{};

        Mode CurrentMode() const { return modes[trial]; }

        void Abort(std::string_view reason)
        {
            SKSE::log::error("stress aborted: {}", reason);
            stage = Stage::Complete;
        }

        void Start(Clock::time_point now)
        {
            *this = {};
            const char* tasklets = std::getenv("APB_STRESS_TASKLETS");
            const std::string_view option = tasklets ? tasklets : "";
            const bool comparePacing = option == "paced";
            taskletAB = option == "1" || comparePacing;
            const char* reverseSetting = std::getenv("APB_STRESS_REVERSE");
            const bool reverse = reverseSetting && std::string_view(reverseSetting) == "1";
            std::string_view order = reverse ? "BAAB" : "ABBA BAAB";
            if (comparePacing) {
                modes = reverse ? reorderedPacedModes : pacedModes;
                order = reverse ? "BCAACB" : "ABCCBA";
            } else if (reverse) {
                modes = reverseModes;
            } else if (taskletAB) {
                modes = taskletModes;
                order = "ABBA";
            }
            start = now;
            result = std::make_shared<Results>();
            frames.reserve(2048);
            nextProbe = now + 7s;
            SKSE::log::info("stress start: {} jobs x {} native slices, {}, test={}", jobs, slices,
                order,
                taskletAB ? "WorkSlice-tasklets" : "budget");
        }

        void Probe(Clock::time_point now)
        {
            if (now >= nextProbe) {
                nextProbe = now + probeInterval;
                if (!Dispatch(result, 0)) {
                    Abort("probe rejected by the VM");
                    return;
                }
                ++probes;
            }
        }

        void Warmup(Clock::time_point now)
        {
            if (!preflightStarted && now >= start + 4s) {
                preflightStarted = true;
                if (!Dispatch(result, 1)) {
                    Abort("preflight work rejected by the VM");
                    return;
                }
            }
            if (now < start + 15s) {
                if (now < start + 14500ms) {
                    Probe(now);
                }
                return;
            }

            const std::lock_guard guard(result->lock);
            if (result->badChecksums != 0 || result->jobMS.size() != 1 ||
                result->probeMS.size() != probes || probes == 0) {
                Abort("preflight script or idle probe did not complete");
                return;
            }
            SKSE::log::info("stress idle: n={} p50_ms={:.3f} p95_ms={:.3f}", probes,
                Percentile(result->probeMS, 0.5), Percentile(result->probeMS, 0.95));
            if (taskletAB) {
                workSlice = FindWorkSlice();
                if (!workSlice || workSlice->CanBeCalledFromTasklets()) {
                    Abort("WorkSlice is missing or not registered in the expected initial mode");
                    return;
                }
            }
            stage = Stage::Settle;
            next = now + 3s;
        }

        bool ReleaseJobs(unsigned completed)
        {
            // A completion racing this snapshot can only leave an unused slot until the next update.
            const auto inFlight = accepted - completed;
            const auto remaining = jobs - accepted;
            const auto count = CurrentMode() == Paced ? std::min(remaining, pacedLimit - inFlight) : remaining;
            if (count != 0) {
                ++releaseUpdates;
                peakReleased = std::max(peakReleased, count);
                peakInFlight = std::max(peakInFlight, inFlight + count);
            }
            for (unsigned job = 0; job < count; ++job) {
                if (!Dispatch(result, slices, taskletAB ? submittedAt : Clock::now())) {
                    Abort("work rejected by the VM");
                    return false;
                }
                ++accepted;
            }
            return true;
        }

        void BeginBurst()
        {
            if (taskletAB) {
                // Only this experiment calls WorkSlice, and all callbacks from the preceding trial are done.
                const bool callable = CurrentMode() != Off;
                workSlice->SetCallableFromTasklets(callable);
                if (workSlice->CanBeCalledFromTasklets() != callable) {
                    Abort("WorkSlice tasklet flag did not match the trial mode");
                    return;
                }
            }
            stage = Stage::Burst;
            result = std::make_shared<Results>();
            result->jobMS.reserve(jobs);
            result->probeMS.reserve(160);
            frames.clear();
            probes = 0;
            accepted = 0;
            releaseUpdates = 0;
            peakReleased = 0;
            peakInFlight = 0;
            boostedFrames = 0;
            policyFrames.fill(0);
            pendingAt1s = -1;
            pendingAt2s = -1;
            budgetSum = 0.0;
            burstStart = Clock::now();
            submittedAt = burstStart;
            if (taskletAB) {
                LARGE_INTEGER qpc{}, frequency{};
                QueryPerformanceCounter(&qpc);
                QueryPerformanceFrequency(&frequency);
                SKSE::log::info("burst capture trial={} tasklets={} qpc={} qpc_hz={} mode={}", trial + 1,
                    workSlice->CanBeCalledFromTasklets(), qpc.QuadPart, frequency.QuadPart, ModeName(CurrentMode()));
            }
            if (!ReleaseJobs(0)) {
                return;
            }
            if (!taskletAB) {
                submittedAt = Clock::now();
            }
            nextProbe = submittedAt;
            if (taskletAB) {
                Probe(submittedAt);
            }
            SKSE::log::info("burst start trial={} on={} accepted={} submit_ms={:.3f} queued={} mode={}", trial + 1,
                CurrentMode() != Off, accepted, Milliseconds(Clock::now() - burstStart), jobs - accepted, ModeName(CurrentMode()));
        }

        void Burst(Clock::time_point now, double frameMS, float baseMS, float appliedMS)
        {
            bool done;
            unsigned completed;
            {
                const std::lock_guard guard(result->lock);
                completed = static_cast<unsigned>(result->jobMS.size());
                const auto pending = jobs - static_cast<int>(completed);
                if (now >= submittedAt + 1s && pendingAt1s < 0) {
                    pendingAt1s = pending;
                }
                if (now >= submittedAt + 2s && pendingAt2s < 0) {
                    pendingAt2s = pending;
                }
                done = pending == 0;
                if (done && result->probeMS.size() == probes) {
                    FinishBurst(now);
                    return;
                }
            }
            if (now >= submittedAt + 40s) {
                Abort("burst exceeded the 40 second timeout");
                return;
            }
            if (!done) {
                frames.push_back(frameMS);
                budgetSum += appliedMS;
                boostedFrames += appliedMS > baseMS ? 1 : 0;
                ++policyFrames[static_cast<std::size_t>(latestPolicy)];
                if (CurrentMode() == Paced && !ReleaseJobs(completed)) {
                    return;
                }
                Probe(now);
            }
        }

        // The caller holds Results::lock so every completion is accounted for.
        void FinishBurst(Clock::time_point now)
        {
            if (accepted != jobs || result->badChecksums || frames.empty() || result->probeMS.empty()) {
                Abort("burst results were incomplete or had a wrong checksum");
                return;
            }
            if (taskletAB && (boostedFrames != 0 || workSlice->CanBeCalledFromTasklets() != (CurrentMode() != Off))) {
                Abort("the tasklet flag or fixed budget changed during the trial");
                return;
            }
            if (CurrentMode() == Paced && (peakReleased > pacedLimit || peakInFlight > pacedLimit)) {
                Abort("paced job limit exceeded");
                return;
            }
            const auto fps = 1000.0 * frames.size() / std::accumulate(frames.begin(), frames.end(), 0.0);
            SKSE::log::info(
                "burst result trial={} on={} jobs={} bad={} probes={} pending1s={} pending2s={} "
                "fps={:.3f} frame_p99_ms={:.3f} budget_mean_ms={:.4f} boosted={}/{} drain_ms={:.3f} "
                "job_p50_ms={:.3f} job_p95_ms={:.3f} probe_p50_ms={:.3f} probe_p95_ms={:.3f} "
                "mode={} release_updates={} peak_released={} peak_inflight={}",
                trial + 1, CurrentMode() != Off, result->jobMS.size(), result->badChecksums, probes,
                std::max(0, pendingAt1s), std::max(0, pendingAt2s), fps, Percentile(frames, 0.99),
                budgetSum / frames.size(), boostedFrames, frames.size(),
                Milliseconds(result->lastJob - submittedAt), Percentile(result->jobMS, 0.5),
                Percentile(result->jobMS, 0.95), Percentile(result->probeMS, 0.5), Percentile(result->probeMS, 0.95),
                ModeName(CurrentMode()), releaseUpdates, peakReleased, peakInFlight);
            std::string samples;
            for (double value : result->probeMS) {
                if (!samples.empty()) {
                    samples += ',';
                }
                samples += std::format("{:.3f}", value);
            }
            SKSE::log::info("burst probe samples trial={} on={} ms={}", trial + 1, CurrentMode() != Off, samples);
            SKSE::log::info("burst policy trial={} disabled={} invalid={} fast={} dwell={} cooldown={} eligible={} max_frame_ms={:.3f}",
                trial + 1, policyFrames[0], policyFrames[1], policyFrames[2], policyFrames[3], policyFrames[4],
                policyFrames[5], *std::ranges::max_element(frames));

            ++trial;
            if (trial == modes.size()) {
                stage = Stage::Complete;
                SKSE::log::info("stress complete");
            } else {
                stage = Stage::Settle;
                next = now + 3s;
            }
        }
    };

    Experiment experiment;
}

void Benchmark::Start(Clock::time_point now)
{
    experiment.Start(now);
}

void Benchmark::Interrupt()
{
    if (experiment.stage == Experiment::Stage::Burst || experiment.stage == Experiment::Stage::Settle) {
        experiment.Abort("gameplay interrupted after measured trials began");
    } else if (experiment.stage == Experiment::Stage::Warmup) {
        experiment.start = {};
    }
}

bool Benchmark::Enabled(Clock::time_point now)
{
    if (experiment.stage == Experiment::Stage::Complete) {
        return false;
    }
    if (experiment.start == Clock::time_point{}) {
        Start(now);
    }
    return !experiment.taskletAB && experiment.stage != Experiment::Stage::Warmup && experiment.CurrentMode() == On;
}

void Benchmark::ObservePolicy(BudgetPolicy::State state)
{
    experiment.latestPolicy = state;
}

void Benchmark::Sample(Clock::time_point now, double frameMS, float baseMS, float appliedMS)
{
    if (frameMS <= 0.0) {
        return;
    }
    switch (experiment.stage) {
    case Experiment::Stage::Warmup:
        experiment.Warmup(now);
        break;
    case Experiment::Stage::Settle:
        if (now >= experiment.next) {
            experiment.BeginBurst();
        }
        break;
    case Experiment::Stage::Burst:
        experiment.Burst(now, frameMS, baseMS, appliedMS);
        break;
    case Experiment::Stage::Complete:
        break;
    }
}
