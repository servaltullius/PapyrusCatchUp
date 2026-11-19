#pragma once

#include "ResumeTrace.h"

namespace NativeDrainTrace
{
    bool Install(ResumeTrace::IsCapturing* capturing, ResumeTrace::Record* recorder, bool compareBudget, bool observeStages,
        bool observeFinish, bool observeBatch, bool observeReleaseSource);
    void BeginTrial(bool enabled);
    void Arm(bool ready);
    void ObserveTaskletEnd(std::uint64_t startedAt, std::uintptr_t returnAddress, std::uintptr_t taskletIdentity);
}
