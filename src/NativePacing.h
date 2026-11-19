#pragma once

namespace NativePacing
{
    inline constexpr unsigned callsPerUpdate = 96;

    bool Install();
    void BeginUpdate(bool allowPromotion);
    void Pause();

#ifdef APB_REAL_TELEMETRY
    struct Counts
    {
        unsigned candidates;
        unsigned promoted;
        unsigned exhausted;
        unsigned peak;
    };

    void ResetCounts();
    Counts ReadCounts();
#endif
}
