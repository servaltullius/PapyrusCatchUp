#pragma once

namespace SKSE
{
    class LoadInterface;
}

// APB_QUEST_PROBE=probe: drives one quest segment of tests/QuestProgressionTestPlan.md several times
// from the same start save and records the order of quest, scene, dialogue and equipment events.
namespace QuestSegmentProbe
{
    // Called from SKSEPluginLoad. Reads the APB_QUEST_* and APB_CATCHUP_* environment variables.
    bool Load(const SKSE::LoadInterface* skse);
}
