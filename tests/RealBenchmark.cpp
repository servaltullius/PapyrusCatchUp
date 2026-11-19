#include "RealBenchmark.h"
#include "NativePacing.h"

#include <algorithm>
#include <array>

namespace
{
    using RealBenchmark::Clock;
    using namespace std::chrono_literals;
    constexpr unsigned jobs = 160;
    constexpr unsigned actors = 48;
    constexpr std::array modes{ false, true, true, false, true, false, false, true };

    double MS(Clock::duration duration)
    {
        return std::chrono::duration<double, std::milli>(duration).count();
    }

    double P95(std::vector<double> values)
    {
        std::ranges::sort(values);
        return values[static_cast<std::size_t>(std::ceil(values.size() * 0.95)) - 1];
    }

    struct Results
    {
        Results()
        {
            expected = "[\n";
            for (unsigned i = 0; i < actors; ++i) {
                expected += "\t20\n";
            }
            expected += ']';
        }

        std::mutex lock;
        std::string expected;
        std::vector<double> probes;
        Clock::time_point last{};
        unsigned completed{};
        unsigned bad{};
        std::string exampleMismatch;
    };

    class Callback final : public RE::BSScript::IStackCallbackFunctor
    {
    public:
        Callback(std::shared_ptr<Results> results, bool probe) :
            results_(std::move(results)), probe_(probe), start_(Clock::now())
        {}

        void operator()(RE::BSScript::Variable value) override
        {
            const auto now = Clock::now();
            const std::lock_guard guard(results_->lock);
            if (probe_) {
                results_->probes.push_back(MS(now - start_));
            } else {
                ++results_->completed;
                results_->last = std::max(now, results_->last);
                if (!value.IsString() || value.GetString() != results_->expected) {
                    if (results_->bad++ == 0) {
                        results_->exampleMismatch = value.IsString() ? value.GetString().substr(0, 50) : "(non-string)";
                    }
                }
            }
        }

        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

    private:
        std::shared_ptr<Results> results_;
        bool probe_;
        Clock::time_point start_;
    };

    bool Dispatch(const std::shared_ptr<Results>& results, bool probe)
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!vm || !player || player->GetFormID() != 0x14) {
            return false;
        }
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{ new Callback(results, probe) };
        if (probe) {
            return vm->DispatchStaticCall("Utility", "Wait", RE::MakeFunctionArguments(0.01f), callback);
        }
        return vm->DispatchStaticCall("AcheronCmd", "ActorVecAsString",
            RE::MakeFunctionArguments(std::vector<RE::Actor*>(actors, player)), callback);
    }

    struct Experiment
    {
        enum class Stage { Warmup, Settle, Burst, Complete };
        Stage stage{ Stage::Warmup };
        Clock::time_point deadline{};
        Clock::time_point submitted{};
        Clock::time_point nextProbe{};
        std::shared_ptr<Results> results;
        unsigned trial{};
        unsigned probeCount{};
        int pendingAt250{ -1 };
        double updateSum{};
        unsigned updateCount{};
        float minBudget{};
        float maxBudget{};

        void Abort(std::string_view reason)
        {
            SKSE::log::error("real benchmark aborted: {}", reason);
            stage = Stage::Complete;
        }

        void StartBurst(Clock::time_point now, float budget)
        {
            results = std::make_shared<Results>();
            stage = Stage::Burst;
            probeCount = 0;
            pendingAt250 = -1;
            updateSum = 0;
            updateCount = 0;
            minBudget = maxBudget = budget;
            submitted = now;
            nextProbe = now;
            NativePacing::ResetCounts();
            LARGE_INTEGER stamp{}, frequency{};
            QueryPerformanceCounter(&stamp);
            QueryPerformanceFrequency(&frequency);
            SKSE::log::info("real capture trial={} on={} qpc={} hz={} jobs={} actors={}",
                trial + 1, modes[trial], stamp.QuadPart, frequency.QuadPart, jobs, actors);
            for (unsigned i = 0; i < jobs; ++i) {
                if (!Dispatch(results, false)) {
                    Abort("the VM rejected AcheronCmd.ActorVecAsString");
                    return;
                }
            }
            Probe(now);
        }

        void Probe(Clock::time_point now)
        {
            if (!Dispatch(results, true)) {
                Abort("the VM rejected the latency probe");
                return;
            }
            ++probeCount;
            nextProbe = now + 250ms;
        }

        bool Step(Clock::time_point now, Clock::duration interval, float budget)
        {
            switch (stage) {
            case Stage::Warmup:
                if (deadline == Clock::time_point{}) {
                    results = std::make_shared<Results>();
                    deadline = now + 15s;
                    if (!Dispatch(results, false)) {
                        Abort("Acheron workload preflight was rejected");
                    }
                } else if (now >= deadline) {
                    const std::lock_guard guard(results->lock);
                    if (results->completed != 1 || results->bad != 0) {
                        SKSE::log::warn("Acheron preflight completed={} bad={} result={:?}",
                            results->completed, results->bad, results->exampleMismatch);
                        Abort("Acheron workload preflight did not return the expected actor list");
                    } else {
                        stage = Stage::Settle;
                        deadline = now + 3s;
                    }
                }
                break;
            case Stage::Settle:
                if (now >= deadline) {
                    StartBurst(now, budget);
                }
                break;
            case Stage::Burst:
                Burst(now, interval, budget);
                break;
            case Stage::Complete:
                break;
            }
            return stage == Stage::Burst && modes[trial];
        }

        void Burst(Clock::time_point now, Clock::duration interval, float budget)
        {
            bool done;
            {
                const std::lock_guard guard(results->lock);
                const auto pending = jobs - results->completed;
                if (now >= submitted + 250ms && pendingAt250 < 0) {
                    pendingAt250 = static_cast<int>(pending);
                }
                done = pending == 0;
                if (done && results->probes.size() == probeCount) {
                    Finish(now);
                    return;
                }
            }
            if (now >= submitted + 60s) {
                Abort("real workload timed out");
                return;
            }
            if (!done) {
                updateSum += MS(interval);
                ++updateCount;
                minBudget = std::min(minBudget, budget);
                maxBudget = std::max(maxBudget, budget);
                if (now >= nextProbe) {
                    Probe(now);
                }
            }
        }

        void Finish(Clock::time_point now)
        {
            const auto counts = NativePacing::ReadCounts();
            if (results->bad != 0 || results->probes.empty() || updateCount == 0 ||
                (modes[trial] != (counts.promoted != 0)) || counts.peak > NativePacing::callsPerUpdate) {
                SKSE::log::warn("real invalid result bad={} probes={} promoted={} peak={}",
                    results->bad, results->probes.size(), counts.promoted, counts.peak);
                Abort("real workload result or native pacing was invalid");
                return;
            }
            SKSE::log::info("real result trial={} on={} jobs={} bad={} pending250={} probes={} "
                "probe_p95_ms={:.3f} first_ms={:.3f} drain_ms={:.3f} vm_fps={:.3f} "
                "budget_min={:.6f} budget_max={:.6f} candidates={} promoted={} fallback={} peak={}",
                trial + 1, modes[trial], results->completed, results->bad, std::max(pendingAt250, 0),
                probeCount, P95(results->probes), results->probes.front(), MS(results->last - submitted),
                1000.0 * updateCount / updateSum, minBudget, maxBudget,
                counts.candidates, counts.promoted, counts.exhausted, counts.peak);
            std::string samples;
            for (double ms : results->probes) {
                if (!samples.empty()) {
                    samples += ',';
                }
                samples += std::format("{:.3f}", ms);
            }
            SKSE::log::info("real probes trial={} ms={}", trial + 1, samples);
            if (++trial == modes.size()) {
                stage = Stage::Complete;
                SKSE::log::info("real benchmark complete");
            } else {
                stage = Stage::Settle;
                deadline = now + 3s;
            }
        }
    };

    Experiment experiment;
}

bool RealBenchmark::Update(Clock::time_point now, Clock::duration interval, float engineBudget)
{
    return experiment.Step(now, interval, engineBudget);
}

void RealBenchmark::Interrupt()
{
    if (experiment.stage == Experiment::Stage::Warmup) {
        experiment.deadline = {};
    } else if (experiment.stage != Experiment::Stage::Complete) {
        experiment.Abort("gameplay was interrupted");
    }
}
