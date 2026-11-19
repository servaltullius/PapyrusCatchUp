#pragma once

#include "RE/B/BSFixedString.h"

#include <cstdint>

namespace RE::BSScript
{
    struct Stack;
}

namespace ResumeTrace
{
    using IsCapturing = bool();
    using Record = void(char, std::uint32_t, std::uint32_t, const RE::BSFixedString*,
        const RE::BSFixedString*, const RE::BSFixedString*, std::uint64_t);

    bool Install(IsCapturing* isCapturing, Record* record, bool compareRetry, bool compareEntry, bool observeRetry,
        bool observeNativeStages, bool observeReleaseSource);
    void BeginTrial(bool retryFirst);
    std::uint64_t TaskletStartedAt(const RE::BSScript::Stack* stack);
}
