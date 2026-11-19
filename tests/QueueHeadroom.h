#pragma once

namespace QueueHeadroom
{
    bool Install();
    bool InstallLimiter();
    bool HasRecentReserve();
    void ObserveGameplay(bool ready);
    void BeginTrial(bool enabled);
    bool EndTrial();
}
