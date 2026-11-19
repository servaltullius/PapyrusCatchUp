// Test-only SKSE plugin for the quest progression test (tests/QuestProgressionTestPlan.md).
//
// APB_QUEST_PROBE=survey: loads every save named in APB_SURVEY_SAVES ('|'-separated base names, the
// first one loaded by APBTestSaveLoader through APB_TEST_SAVE) and writes one block per save with the
// state of the quests the test segments start from. Nothing is written to the game; the run script
// deletes any save it copied into the test profile.
//
// APB_QUEST_PROBE=probe: drives a quest segment and records its events; see QuestSegmentProbe.cpp.
#include "QuestProbeCommon.h"
#include "QuestSegmentProbe.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <format>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using namespace std::chrono_literals;

    constexpr auto runtime = SKSE::RUNTIME_SSE_1_6_1170;

    struct SurveyQuest
    {
        std::uint32_t id;
        const char* editorID;
    };

    // Skyrim.esm quests: the main quest around Alduin's Wall, Arniel's Endeavor and the College line
    // that gates it. Editor IDs are checked against the loaded forms, so a wrong ID shows in the log.
    constexpr std::array<SurveyQuest, 16> surveyQuests{ {
        { 0x035D5F, "MQ201" },
        { 0x036191, "MQ202" },
        { 0x036192, "MQ203" },
        { 0x02D515, "MQ204" },
        { 0x01F251, "MG01" },
        { 0x01F252, "MG02" },
        { 0x01F253, "MG03" },
        { 0x01F254, "MG04" },
        { 0x01F255, "MG05" },
        { 0x01F256, "mg06" },
        { 0x01F257, "MG07" },
        { 0x01F258, "MG08" },
        { 0x06A086, "MGRArniel01" },
        { 0x06A08D, "MGRArniel02" },
        { 0x06A097, "MGRArniel03" },
        { 0x06A119, "MGRArniel04" },
    } };

    // GetStageDone is evaluated for every index in this range; vanilla stage indices of these quests
    // stay below 1000.
    constexpr std::uint16_t maxSurveyStage = 1000;

    std::vector<std::string> surveySaves;
    std::atomic_size_t       surveyIndex{};
    std::jthread             worker;

    using QuestProbe::FormLabel;

    std::string DescribeReference(RE::TESObjectREFR* reference)
    {
        if (!reference) {
            return "empty";
        }
        std::string text = std::format("{:08X}", reference->GetFormID());
        if (const auto* base = reference->GetBaseObject()) {
            text += std::format("[{}]", base->GetName());
        }
        text += std::format(" disabled={}", reference->IsDisabled());
        if (const auto* actor = reference->As<RE::Actor>()) {
            text += std::format(" dead={}", actor->IsDead());
        }
        text += std::format(" cell={} location={}", FormLabel(reference->GetParentCell()), FormLabel(reference->GetCurrentLocation()));
        return text;
    }

    void LogQuest(const SurveyQuest& entry, RE::PlayerCharacter* player)
    {
        auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(entry.id);
        if (!quest) {
            SKSE::log::error("survey quest {} {:08X}: form missing", entry.editorID, entry.id);
            return;
        }
        const char* editorID = quest->GetFormEditorID();
        if (!editorID || std::string_view(editorID) != entry.editorID) {
            SKSE::log::error("survey quest {:08X}: editor ID {} does not match the expected {}", entry.id, editorID ? editorID : "", entry.editorID);
            return;
        }

        const std::string done = QuestProbe::DoneStages(quest, maxSurveyStage, player);
        // TESQuest::IsRunning reports true for stopped quests on this runtime, and the run-time stage
        // list does not hold one entry per stage, so neither is logged: 'stopped' and GetStageDone
        // are the state.
        SKSE::log::info("survey quest {} stopped={} starting={} stopping={} completed={} already_run={} stage={} done=[{}]",
            entry.editorID, quest->IsStopped(), quest->IsStarting(), quest->IsStopping(), quest->IsCompleted(), quest->alreadyRun,
            quest->GetCurrentStageID(), done);

        std::string objectives;
        for (const auto* objective : quest->objectives) {
            if (objective) {
                objectives += std::format("{}{}:{}", objectives.empty() ? "" : ",", objective->index, std::to_underlying(objective->state.get()));
            }
        }
        if (!objectives.empty()) {
            SKSE::log::info("survey objectives {} [{}] (1 displayed, 2 completed, 3 completed+displayed, 4 failed, 5 failed+displayed)", entry.editorID, objectives);
        }

        for (const auto* alias : quest->aliases) {
            if (!alias || alias->GetVMTypeID() != RE::BGSRefAlias::VMTYPEID) {
                continue;
            }
            RE::NiPointer<RE::TESObjectREFR> reference;
            if (const auto it = quest->refAliasMap.find(alias->aliasID); it != quest->refAliasMap.end()) {
                reference = it->second.get();
            }
            if (reference) {
                SKSE::log::info("survey alias {}.{} {}", entry.editorID, alias->aliasName.c_str(), DescribeReference(reference.get()));
            }
        }
    }

    // Main thread.
    void LogSurvey(std::size_t index)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            SKSE::log::error("survey save={} index={}: player unavailable", surveySaves[index], index);
            return;
        }
        const auto* calendar = RE::Calendar::GetSingleton();
        SKSE::log::info("survey save={} index={} player={} level={} cell={} location={} days_passed={:.2f}", surveySaves[index], index,
            player->GetName(), player->GetLevel(), FormLabel(player->GetParentCell()), FormLabel(player->GetCurrentLocation()),
            calendar ? calendar->GetDaysPassed() : -1.0f);
        for (const auto& entry : surveyQuests) {
            LogQuest(entry, player);
        }
        SKSE::log::info("survey save={} done", surveySaves[index]);
    }

    void AdvanceSurvey(std::size_t index)
    {
        const auto next = index + 1;
        if (next >= surveySaves.size()) {
            SKSE::log::info("quest survey complete saves={}", surveySaves.size());
            return;
        }
        surveyIndex.store(next);
        SKSE::GetTaskInterface()->AddTask([next] {
            auto* manager = RE::BGSSaveLoadManager::GetSingleton();
            if (!manager) {
                SKSE::log::error("quest survey aborted: save manager unavailable before save={}", surveySaves[next]);
                return;
            }
            // Load reports nothing, and a name the game does not know would never send a load
            // message, so the name is checked against the save list first.
            manager->PopulateSaveList();
            for (const auto* entry : manager->saveGameList) {
                if (entry && surveySaves[next] == entry->fileName.c_str()) {
                    SKSE::log::info("survey loading save={} index={}", surveySaves[next], next);
                    manager->Load(surveySaves[next].c_str());
                    return;
                }
            }
            SKSE::log::error("survey load failed save={} index={}: not in the active profile", surveySaves[next], next);
            AdvanceSurvey(next);
        });
    }

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        if (message->type == SKSE::MessagingInterface::kPreLoadGame) {
            const auto* name = static_cast<const char*>(message->data);
            SKSE::log::info("survey pre-load file={}", name ? std::string_view(name, message->dataLen) : std::string_view{});
        } else if (message->type == SKSE::MessagingInterface::kPostLoadGame) {
            const auto index = surveyIndex.load();
            if (!message->data) {
                SKSE::log::error("survey load failed save={} index={}", surveySaves[index], index);
                AdvanceSurvey(index);
                return;
            }
            // Past the first frames after a load, when scripts registered for OnPlayerLoadGame and
            // quest start-up run; the state read below is what the save holds once those settle.
            worker = std::jthread([index](std::stop_token stop) {
                std::this_thread::sleep_for(5s);
                if (stop.stop_requested()) {
                    return;
                }
                SKSE::GetTaskInterface()->AddTask([index] {
                    LogSurvey(index);
                    AdvanceSurvey(index);
                });
            });
        }
    }
}

SKSE_PLUGIN_VERSION = [] {
    SKSE::PluginVersionData info{};
    info.PluginName("APBQuestProbe");
    info.PluginVersion({ 0, 1, 0, 0 });
    info.CompatibleVersions({ runtime });
    info.UsesUpdatedStructs();
    return info;
}();

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    if (skse->RuntimeVersion() != runtime) {
        return false;
    }
    const char* mode = std::getenv("APB_QUEST_PROBE");
    if (mode && std::string_view(mode) == "probe") {
        return QuestSegmentProbe::Load(skse);
    }
    if (!mode || std::string_view(mode) != "survey") {
        return true;
    }
    SKSE::Init(skse, { .logName = "APBQuestProbe" });
    if (const char* saves = std::getenv("APB_SURVEY_SAVES")) {
        std::string_view list(saves);
        while (!list.empty()) {
            const auto split = list.find('|');
            const auto name = list.substr(0, split);
            if (!name.empty()) {
                surveySaves.emplace_back(name);
            }
            list = split == std::string_view::npos ? std::string_view{} : list.substr(split + 1);
        }
    }
    if (surveySaves.empty()) {
        SKSE::log::error("quest survey aborted: APB_SURVEY_SAVES is empty");
        return true;
    }
    SKSE::log::info("quest survey saves={}", surveySaves.size());
    return SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
}
