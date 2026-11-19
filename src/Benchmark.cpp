#include "Benchmark.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace
{
    std::atomic_bool probePending{};
    std::atomic_uint generation{};

    class Probe final : public RE::BSScript::IStackCallbackFunctor
    {
    public:
        Probe(unsigned phase, unsigned epoch, bool enabled, double fps, float budgetMS) :
            phase_(phase), epoch_(epoch), enabled_(enabled), fps_(fps),
            budgetMS_(budgetMS), start_(Benchmark::Clock::now())
        {}

        ~Probe() override { probePending.store(false); }

        void operator()(RE::BSScript::Variable) override
        {
            if (epoch_ == generation.load()) {
                const auto elapsed = std::chrono::duration<double, std::milli>(Benchmark::Clock::now() - start_);
                SKSE::log::info("probe phase={} on={} fps={:.2f} budget_ms={:.4f} latency_ms={:.4f}",
                    phase_, enabled_, fps_, budgetMS_, elapsed.count());
            }
        }

        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

    private:
        unsigned phase_;
        unsigned epoch_;
        bool enabled_;
        double fps_;
        float budgetMS_;
        Benchmark::Clock::time_point start_;
    };
}

void Benchmark::Start(Clock::time_point now)
{
    ++generation;
    start_ = now;
    phase_ = 0;
    nextProbe_ = now;
    finished_ = false;
    frameTimes_.clear();
    frameTimes_.reserve(4096);
    SKSE::log::info("benchmark start: ABBA, 30 s per phase, first 6 s excluded");
}

void Benchmark::Interrupt()
{
    if (start_ != Clock::time_point{} && !finished_) {
        ++generation;
        start_ = {};
        SKSE::log::info("benchmark interrupted; starting a fresh run when gameplay resumes");
    }
}

bool Benchmark::Enabled(Clock::time_point now)
{
    if (finished_) {
        return true;
    }
    if (start_ == Clock::time_point{}) {
        Start(now);
    }
    const auto nextPhase = static_cast<unsigned>((now - start_) / phaseLength);
    if (nextPhase != phase_) {
        if (!frameTimes_.empty()) {
            std::ranges::sort(frameTimes_);
            const auto p99Index = static_cast<std::size_t>(std::ceil(frameTimes_.size() * 0.99)) - 1;
            const double sum = std::accumulate(frameTimes_.begin(), frameTimes_.end(), 0.0);
            SKSE::log::info("frames phase={} on={} n={} fps={:.3f} p99_ms={:.3f}",
                phase_ + 1, modes[phase_], frameTimes_.size(), 1000.0 * frameTimes_.size() / sum,
                frameTimes_[p99Index]);
        }
        frameTimes_.clear();
        ++generation;
        if (nextPhase >= modes.size()) {
            finished_ = true;
            SKSE::log::info("benchmark complete");
            return true;
        }
        phase_ = nextPhase;
    }
    return modes[phase_];
}

void Benchmark::Sample(Clock::time_point now, double frameMS, float baseMS, float appliedMS)
{
    if (finished_ || start_ == Clock::time_point{} || frameMS <= 0.0) {
        return;
    }
    const auto phaseElapsed = now - start_ - phaseLength * phase_;
    if (phaseElapsed < warmup) {
        return;
    }
    frameTimes_.push_back(frameMS);
    if (now < nextProbe_ || phaseElapsed >= phaseLength - std::chrono::seconds(2)) {
        return;
    }
    nextProbe_ = now + std::chrono::seconds(1);
    if (probePending.exchange(true)) {
        SKSE::log::warn("probe skipped: previous VM call pending");
        return;
    }
    auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    if (!vm) {
        probePending.store(false);
        return;
    }
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result{
        new Probe(phase_ + 1, generation.load(), modes[phase_], 1000.0 / frameMS, appliedMS)
    };
    if (!vm->DispatchStaticCall("Utility", "Wait", RE::MakeFunctionArguments(0.01f), result)) {
        SKSE::log::warn("Utility.Wait probe dispatch rejected");
    }
    if (modes[phase_] && appliedMS <= baseMS) {
        SKSE::log::info("probe phase={} had no budget increase at {:.2f} FPS", phase_ + 1, 1000.0 / frameMS);
    }
}
