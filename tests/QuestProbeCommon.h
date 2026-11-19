#pragma once

#include <format>
#include <string>

// Helpers shared by the quest survey and the quest segment probe (APBQuestProbe).
namespace QuestProbe
{
    inline std::string FormLabel(const RE::TESForm* form)
    {
        if (!form) {
            return "none";
        }
        const char* editorID = form->GetFormEditorID();
        return editorID && editorID[0] ? std::format("{:08X}({})", form->GetFormID(), editorID) : std::format("{:08X}", form->GetFormID());
    }

    // The engine's own condition function, so "done" means exactly what scripts and dialogue see.
    // TESQuest::IsRunning reports true for stopped quests on this runtime, and the run-time stage list
    // does not hold one entry per stage, so neither is used.
    inline bool StageDone(RE::TESQuest* quest, std::uint16_t stage, RE::PlayerCharacter* player)
    {
        RE::TESConditionItem condition;
        condition.data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetStageDone;
        condition.data.functionData.params[0] = quest;
        condition.data.functionData.params[1] = reinterpret_cast<void*>(static_cast<std::uintptr_t>(stage));
        condition.data.comparisonValue.f = 1.0f;
        condition.data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kEqualTo;
        condition.data.object = RE::CONDITIONITEMOBJECT::kSelf;
        RE::ConditionCheckParams params(player, nullptr);
        return condition.IsTrue(params);
    }

    // Comma-separated indices of the stages in [0, last] that are done.
    inline std::string DoneStages(RE::TESQuest* quest, std::uint16_t last, RE::PlayerCharacter* player)
    {
        std::string done;
        for (std::uint16_t stage = 0; stage <= last; ++stage) {
            if (StageDone(quest, stage, player)) {
                done += std::format("{}{}", done.empty() ? "" : ",", stage);
            }
        }
        return done;
    }
}
