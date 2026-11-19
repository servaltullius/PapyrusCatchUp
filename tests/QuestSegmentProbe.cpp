// Test-only: the "probe" mode of APBQuestProbe (tests/QuestProgressionTestPlan.md, sections 6-8).
//
// Segment A, MQ203 "Alduin's Wall" from stage 10 to the end of the Sleeping Giant Inn scene: each trial
// starts from the same save, brings the player and Esbern to Riverwood and then into the inn through
// the inn door's own teleport data, and from there only watches. Esbern's alias script sets stages 20
// and 30 on his location changes, stage 30 starts MQ203RiverwoodScene, and the scene and the quest
// script set 40, 48, 50 and 60.
//
// Everything is written as rows of APBQuestProbe.tsv: trial, ms since the trial began, render frame,
// thread, kind, two numbers and a text. Event sinks only append rows; they run on whichever thread
// raised the event (a stage set by a script arrives on the VM job thread, which may be inside an extra
// VM cycle), so they never call into VMCatchUp. A worker thread drives the trial and reads the VM
// catch-up counters once a second ("cu" rows), which is what shows whether extra cycles were running
// around an event.
#include "QuestSegmentProbe.h"

#include "MenuWatch.h"
#include "QuestProbeCommon.h"
#include "VMCatchUp.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    using QuestProbe::DoneStages;
    using QuestProbe::FormLabel;
    using QuestProbe::StageDone;

    // Skyrim.esm forms of segment A. Quests, scene and factions are checked by editor ID when a trial
    // starts; the actors come from MQ203's aliases, and the door outside the inn from the teleport
    // link of the door inside it.
    constexpr RE::FormID mq202ID = 0x00036191;
    constexpr RE::FormID mq203ID = 0x00036192;
    constexpr RE::FormID riverwoodSceneID = 0x00037BDF;
    constexpr RE::FormID innDoorID = 0x00013419;  // inside RiverwoodSleepingGiantInn
    constexpr RE::FormID playerFollowerFactionID = 0x00084D1B;
    constexpr RE::FormID bladesFactionID = 0x00072834;
    constexpr RE::FormID playerID = 0x00000014;

    constexpr std::uint16_t lastSnapshotStage = 300;
    constexpr std::size_t maxRows = 400000;
    constexpr auto arrivalLimit = 120s;
    constexpr auto stageTriggerLimit = 30s;
    // From stage 30 to stage 60 and the scene's waiting phase. Without the plugin, at 30 FPS, stage 60
    // came 177 to 216 s after stage 30 and the waiting phase about 16 s later.
    constexpr auto          sceneLimit = 420s;
    constexpr std::uint32_t waitingPhase = 29;
    constexpr auto          waitingPhaseLimit = 60s;
    constexpr auto loadLimit = 180s;

    struct Config
    {
        unsigned    trials{ 5 };
        bool        midSave{};
        bool        catchUp{};
        // Seconds Esbern waits outside after the player entered the inn. Zero starts the scene while
        // the plugin's 10 s hold after a loading screen is still running, as in normal play; 12 puts
        // the whole scene after the hold.
        double      entryDelay{};
        std::string startSave;
    };

    Config config;

    // Rows -------------------------------------------------------------------------------------

    struct Row
    {
        double        ms;
        std::uint32_t frame;
        std::uint32_t thread;
        std::string   kind;
        std::uint32_t a;
        std::uint32_t b;
        std::string   text;
    };

    std::mutex              rowLock;
    std::vector<Row>        rows;
    std::size_t             droppedRows{};
    std::atomic_bool        recording{};
    std::atomic<Clock::rep> trialStart{};
    std::filesystem::path   rowFile;

    std::uint32_t Frame()
    {
        auto* state = RE::BSGraphics::State::GetSingleton();
        return state ? state->GetFrameCount() : 0;
    }

    double TrialMS()
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - Clock::time_point(Clock::duration(trialStart.load()))).count();
    }

    void Record(std::string_view kind, std::uint32_t a = 0, std::uint32_t b = 0, std::string text = {})
    {
        if (!recording.load(std::memory_order_acquire)) {
            return;
        }
        Row row{ TrialMS(), Frame(), GetCurrentThreadId(), std::string(kind), a, b, std::move(text) };
        const std::lock_guard guard(rowLock);
        if (rows.size() < maxRows) {
            rows.push_back(std::move(row));
        } else {
            ++droppedRows;
        }
    }

    // Appends the trial's rows to the file and returns how many were dropped for lack of room.
    std::size_t FlushRows(unsigned trial)
    {
        std::vector<Row> out;
        std::size_t      dropped = 0;
        {
            const std::lock_guard guard(rowLock);
            out.swap(rows);
            dropped = std::exchange(droppedRows, 0);
        }
        std::ofstream file(rowFile, std::ios::app);
        for (auto& row : out) {
            for (auto& c : row.text) {
                if (c == '\t' || c == '\n' || c == '\r') {
                    c = ' ';
                }
            }
            file << std::format("{}\t{:.3f}\t{}\t{}\t{}\t{:08X}\t{:08X}\t{}\n", trial, row.ms, row.frame, row.thread, row.kind, row.a, row.b, row.text);
        }
        return dropped;
    }

    // Trial state the sinks write ------------------------------------------------------------------

    constexpr std::array<std::uint16_t, 6> watchedStages{ 20, 30, 40, 48, 50, 60 };
    std::array<std::atomic_bool, watchedStages.size()> stageSeen{};
    std::atomic<RE::FormID> esbernID{};
    std::atomic<RE::FormID> delphineID{};
    std::atomic<RE::FormID> orgnarID{};
    std::atomic<RE::BGSScene*> scenePointer{};

    bool Seen(std::uint16_t stage)
    {
        for (std::size_t i = 0; i < watchedStages.size(); ++i) {
            if (watchedStages[i] == stage) {
                return stageSeen[i].load();
            }
        }
        return false;
    }

    bool SceneActor(RE::FormID id)
    {
        return id != 0 && (id == esbernID.load() || id == delphineID.load() || id == orgnarID.load());
    }

    class StageSink final : public RE::BSTEventSink<RE::TESQuestStageEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStageEvent* event, RE::BSTEventSource<RE::TESQuestStageEvent>*) override
        {
            if (event && (event->formID == mq203ID || event->formID == mq202ID)) {
                Record("stage", event->formID, event->stage);
                if (event->formID == mq203ID) {
                    for (std::size_t i = 0; i < watchedStages.size(); ++i) {
                        if (watchedStages[i] == event->stage) {
                            stageSeen[i].store(true);
                        }
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class QuestSink final : public RE::BSTEventSink<RE::TESQuestStartStopEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStartStopEvent* event, RE::BSTEventSource<RE::TESQuestStartStopEvent>*) override
        {
            if (event && (event->formID == mq203ID || event->formID == mq202ID)) {
                Record("quest", event->formID, event->started, event->started ? "start" : "stop");
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class TopicSink final : public RE::BSTEventSink<RE::TESTopicInfoEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESTopicInfoEvent* event, RE::BSTEventSource<RE::TESTopicInfoEvent>*) override
        {
            if (event && event->speakerRef && SceneActor(event->speakerRef->GetFormID())) {
                Record("topic", event->speakerRef->GetFormID(), event->topicInfoFormID,
                    event->type.get() == RE::TESTopicInfoEvent::TopicInfoEventType::kTopicBegin ? "begin" : "end");
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class EquipSink final : public RE::BSTEventSink<RE::TESEquipEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* event, RE::BSTEventSource<RE::TESEquipEvent>*) override
        {
            if (event && event->actor && SceneActor(event->actor->GetFormID())) {
                Record("equip", event->actor->GetFormID(), event->baseObject, event->equipped ? "equip" : "unequip");
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class LocationSink final : public RE::BSTEventSink<RE::TESActorLocationChangeEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESActorLocationChangeEvent* event, RE::BSTEventSource<RE::TESActorLocationChangeEvent>*) override
        {
            if (event && event->actor) {
                const auto id = event->actor->GetFormID();
                if (id == playerID || SceneActor(id)) {
                    Record("loc", id, event->newLoc ? event->newLoc->GetFormID() : 0,
                        std::format("from={:08X}", event->oldLoc ? event->oldLoc->GetFormID() : 0));
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // On Esbern's animation graph. MQ203 registers for "AnimObjectUnequip" at stage 40 and sets stage
    // 48 when it arrives; this records when the graph actually raised it.
    class AnimSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
        {
            if (event && event->tag.c_str() && std::string_view(event->tag.c_str()).starts_with("AnimObject")) {
                Record("anim", event->holder ? event->holder->GetFormID() : 0, 0, event->tag.c_str());
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    StageSink    stageSink;
    QuestSink    questSink;
    TopicSink    topicSink;
    EquipSink    equipSink;
    LocationSink locationSink;
    AnimSink     animSink;

    // Gate of the VM catch-up module. The menu part is the release plugin's own code (MenuWatch.h).

    std::atomic_bool gameplayReady{};
    std::atomic_uint loadGeneration{};
    std::atomic_uint failedLoads{};
    std::atomic_uint savesStarted{};

    bool Gate()
    {
        return gameplayReady.load() && !MenuWatch::Blocked();
    }

    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (event) {
                // Every menu, so that a dialogue, inventory or console opened by stray input shows.
                Record("menu", 0, event->opening, std::format("{} {}", event->menuName.c_str(), event->opening ? "open" : "close"));
                if (event->menuName == RE::LoadingMenu::MENU_NAME && !event->opening && config.catchUp) {
                    VMCatchUp::NotifyGameLoaded();  // the release plugin starts its post-load hold here
                }
            }
            MenuWatch::OnEvent(event);
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    MenuSink menuSink;

    bool InstallCatchUp()
    {
        VMCatchUp::Settings settings{};
        settings.mode = VMCatchUp::Mode::kAccelerate;
        if (const char* mode = std::getenv("APB_CATCHUP_MODE")) {
            const std::string_view name{ mode };
            if (name != "accelerate" && name != "carried") {
                SKSE::log::error("quest probe aborted: unknown APB_CATCHUP_MODE '{}'", name);
                return false;
            }
            settings.mode = name == "accelerate" ? VMCatchUp::Mode::kAccelerate : VMCatchUp::Mode::kCarried;
        }
        if (const char* target = std::getenv("APB_CATCHUP_TARGET")) {
            settings.targetHz = std::strtod(target, nullptr);
        }
        if (const char* engage = std::getenv("APB_CATCHUP_ENGAGE")) {
            settings.engageHz = std::strtod(engage, nullptr);
        }
        settings.logIntervalSeconds = 0;  // the worker reads the counters itself
        settings = VMCatchUp::Sanitize(settings);
        if (!VMCatchUp::Install(settings, Gate)) {
            SKSE::log::error("quest probe aborted: VM catch-up did not install");
            return false;
        }
        SKSE::log::info("quest probe catch-up mode={} target_hz={:.0f} engage_hz={:.0f}",
            settings.mode == VMCatchUp::Mode::kAccelerate ? "accelerate" : "carried", settings.targetHz, settings.engageHz);
        return true;
    }

    // SKSE tasks ------------------------------------------------------------------------------------

    // Runs fn as an SKSE task and returns its result, or nothing when the game did not get to it.
    // Not always on the same thread: the rows of one trial came from six.
    template <class F>
    auto Ask(F fn, std::chrono::seconds timeout = 60s) -> std::optional<std::invoke_result_t<F>>
    {
        using T = std::invoke_result_t<F>;
        auto promise = std::make_shared<std::promise<T>>();
        auto future = promise->get_future();
        SKSE::GetTaskInterface()->AddTask([fn = std::move(fn), promise]() mutable { promise->set_value(fn()); });
        if (future.wait_for(timeout) != std::future_status::ready) {
            return std::nullopt;
        }
        return future.get();
    }

    struct World
    {
        RE::PlayerCharacter* player{};
        RE::TESQuest*        mq202{};
        RE::TESQuest*        mq203{};
        RE::BGSScene*        scene{};
        RE::Actor*           esbern{};
        RE::Actor*           delphine{};
        RE::Actor*           orgnar{};
        RE::TESObjectREFR*   innDoor{};
        RE::TESObjectREFR*   outsideDoor{};
        RE::TESFaction*      follower{};
        RE::TESFaction*      blades{};

        // Orgnar only takes part in two optional phases of the scene.
        [[nodiscard]] bool Complete() const
        {
            return player && mq202 && mq203 && scene && esbern && delphine && innDoor && outsideDoor && follower && blades;
        }
    };

    bool SameName(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
    }

    RE::Actor* AliasActor(RE::TESQuest* quest, std::string_view name)
    {
        for (const auto* alias : quest->aliases) {
            if (!alias || !SameName(alias->aliasName.c_str(), name)) {
                continue;
            }
            if (const auto it = quest->refAliasMap.find(alias->aliasID); it != quest->refAliasMap.end()) {
                // The alias actors are persistent unique NPCs, so the plain pointer stays valid.
                const auto reference = it->second.get();
                return reference ? reference->As<RE::Actor>() : nullptr;
            }
        }
        return nullptr;
    }

    const RE::DoorTeleportData* TeleportOf(RE::TESObjectREFR* door)
    {
        const auto* extra = door ? door->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
        return extra ? extra->teleportData : nullptr;
    }

    bool HasEditorID(const RE::TESForm* form, std::string_view expected)
    {
        const char* editorID = form ? form->GetFormEditorID() : nullptr;
        return editorID && SameName(editorID, expected);
    }

    World Resolve()
    {
        World world;
        world.player = RE::PlayerCharacter::GetSingleton();
        world.mq202 = RE::TESForm::LookupByID<RE::TESQuest>(mq202ID);
        world.mq203 = RE::TESForm::LookupByID<RE::TESQuest>(mq203ID);
        if (!HasEditorID(world.mq202, "MQ202")) {
            world.mq202 = nullptr;
        }
        if (!HasEditorID(world.mq203, "MQ203")) {
            world.mq203 = nullptr;
        }
        world.scene = RE::TESForm::LookupByID<RE::BGSScene>(riverwoodSceneID);
        if (world.scene && world.scene->parentQuest != world.mq203) {
            world.scene = nullptr;
        }
        if (world.mq203) {
            world.esbern = AliasActor(world.mq203, "esbern");
            world.delphine = AliasActor(world.mq203, "delphine");
            world.orgnar = AliasActor(world.mq203, "orgnar");
        }
        world.innDoor = RE::TESForm::LookupByID<RE::TESObjectREFR>(innDoorID);
        if (const auto* teleport = TeleportOf(world.innDoor)) {
            world.outsideDoor = teleport->linkedDoor.get().get();  // a load door's partner is persistent
        }
        world.follower = RE::TESForm::LookupByID<RE::TESFaction>(playerFollowerFactionID);
        world.blades = RE::TESForm::LookupByID<RE::TESFaction>(bladesFactionID);
        esbernID.store(world.esbern ? world.esbern->GetFormID() : 0);
        delphineID.store(world.delphine ? world.delphine->GetFormID() : 0);
        orgnarID.store(world.orgnar ? world.orgnar->GetFormID() : 0);
        scenePointer.store(world.scene);
        return world;
    }

    bool InGameplay()
    {
        auto* ui = RE::UI::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        return ui && player && !ui->GameIsPaused() && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) &&
            !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) && player->GetParentCell() && player->Is3DLoaded();
    }

    // Places the player where activating this load door would: beside the door it leads to.
    bool SendPlayerThrough(RE::TESObjectREFR* door)
    {
        const auto* teleport = TeleportOf(door);
        auto*       player = RE::PlayerCharacter::GetSingleton();
        if (!teleport || !player) {
            return false;
        }
        const auto target = teleport->linkedDoor.get();
        if (!target) {
            return false;
        }
        // TESObjectREFR::MoveTo's implementation (AE ID 56626), with the door's arrival point instead
        // of the target reference's own position.
        using MoveFn = void(RE::TESObjectREFR*, const RE::ObjectRefHandle&, RE::TESObjectCELL*, RE::TESWorldSpace*, const RE::NiPoint3&, const RE::NiPoint3&);
        static REL::Relocation<MoveFn> move{ REL::ID(56626) };
        const RE::ObjectRefHandle handle = target->GetHandle();
        move(player, handle, target->GetParentCell(), target->GetWorldspace(), teleport->position, teleport->rotation);
        return true;
    }

    std::string DescribeActor(const World& world, RE::Actor* actor)
    {
        if (!actor) {
            return "missing";
        }
        const auto* package = actor->GetCurrentPackage();
        return std::format("{:08X} cell={} location={} disabled={} dead={} loaded={} follower={} blades={} waiting={:.0f} left={} right={} package={}",
            actor->GetFormID(), FormLabel(actor->GetParentCell()), FormLabel(actor->GetCurrentLocation()), actor->IsDisabled(), actor->IsDead(),
            actor->Is3DLoaded(), actor->IsInFaction(world.follower), actor->IsInFaction(world.blades),
            actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kWaitingForPlayer), FormLabel(actor->GetEquippedObject(true)),
            FormLabel(actor->GetEquippedObject(false)), FormLabel(package));
    }

    // One "snap" row per line, each starting with the label. Positions and game time are left out so
    // that two runs that ended in the same state give the same text.
    bool Snapshot(const char* label)
    {
        const World world = Resolve();
        if (!world.Complete()) {
            Record("snap", 0, 0, std::format("{};unresolved", label));
            return false;
        }
        auto* player = world.player;
        Record("snap", mq203ID, world.mq203->GetCurrentStageID(),
            std::format("{};MQ203 stage={} done=[{}] stopped={} completed={}", label, world.mq203->GetCurrentStageID(),
                DoneStages(world.mq203, lastSnapshotStage, player), world.mq203->IsStopped(), world.mq203->IsCompleted()));
        std::string objectives;
        for (const auto* objective : world.mq203->objectives) {
            if (objective) {
                objectives += std::format("{}{}:{}", objectives.empty() ? "" : ",", objective->index, std::to_underlying(objective->state.get()));
            }
        }
        Record("snap", mq203ID, 0, std::format("{};MQ203 objectives=[{}]", label, objectives));
        Record("snap", mq202ID, world.mq202->GetCurrentStageID(),
            std::format("{};MQ202 stage={} stopped={} completed={}", label, world.mq202->GetCurrentStageID(), world.mq202->IsStopped(), world.mq202->IsCompleted()));
        Record("snap", riverwoodSceneID, world.scene->currentPhaseIndex,
            std::format("{};scene playing={} phase={}", label, world.scene->isPlaying, static_cast<std::int32_t>(world.scene->currentPhaseIndex)));
        std::string aliases;
        for (const auto* alias : world.mq203->aliases) {
            if (!alias) {
                continue;
            }
            std::uint32_t filled = 0;
            if (const auto it = world.mq203->refAliasMap.find(alias->aliasID); it != world.mq203->refAliasMap.end()) {
                if (const auto reference = it->second.get()) {
                    filled = reference->GetFormID();
                }
            }
            aliases += std::format("{}{}:{:08X}", aliases.empty() ? "" : ",", alias->aliasName.c_str(), filled);
        }
        Record("snap", mq203ID, 0, std::format("{};MQ203 aliases=[{}]", label, aliases));
        Record("snap", esbernID.load(), 0, std::format("{};esbern {}", label, DescribeActor(world, world.esbern)));
        Record("snap", delphineID.load(), 0, std::format("{};delphine {}", label, DescribeActor(world, world.delphine)));
        Record("snap", playerID, 0, std::format("{};player cell={} location={} blades={}", label, FormLabel(player->GetParentCell()),
            FormLabel(player->GetCurrentLocation()), player->IsInFaction(world.blades)));
        // Stage 60 of MQ203 sets this quest to stage 50.
        if (auto* mound = RE::TESForm::LookupByEditorID<RE::TESQuest>("dunDragonMoundQST")) {
            Record("snap", mound->GetFormID(), mound->GetCurrentStageID(), std::format("{};dunDragonMoundQST stage={}", label, mound->GetCurrentStageID()));
        } else {
            Record("snap", 0, 0, std::format("{};dunDragonMoundQST not found by editor ID", label));
        }
        return true;
    }

    // Where the player and the scene's actors are. Not part of any comparison: it shows whether the
    // player was moved by input during a trial and where an actor stood when a scene phase stalled.
    void RecordPositions()
    {
        const World world = Resolve();
        const auto  row = [](RE::TESObjectREFR* reference) {
            if (reference) {
                const auto  position = reference->GetPosition();
                const auto* cell = reference->GetParentCell();
                Record("pos", reference->GetFormID(), cell ? cell->GetFormID() : 0,
                    std::format("x={:.0f} y={:.0f} z={:.0f}", position.x, position.y, position.z));
            }
        };
        row(world.player);
        if (world.player) {
            // The player only runs a package when something drives them, for example an idle-sandbox mod.
            const auto* package = world.player->GetCurrentPackage();
            Record("pkg", playerID, package ? package->GetFormID() : 0, FormLabel(package));
        }
        row(world.esbern);
        row(world.delphine);
        row(world.orgnar);
    }

    // Worker thread --------------------------------------------------------------------------------

    bool Pause(std::stop_token stop, Clock::duration duration)
    {
        const auto until = Clock::now() + duration;
        while (Clock::now() < until && !stop.stop_requested()) {
            std::this_thread::sleep_for(50ms);
        }
        return !stop.stop_requested();
    }

    template <class P>
    bool WaitUntil(std::stop_token stop, Clock::duration limit, P predicate)
    {
        const auto until = Clock::now() + limit;
        while (!stop.stop_requested()) {
            if (predicate()) {
                return true;
            }
            if (Clock::now() >= until) {
                return false;
            }
            std::this_thread::sleep_for(100ms);
        }
        return false;
    }

    bool WaitGameplay(std::stop_token stop, Clock::duration limit)
    {
        return WaitUntil(stop, limit, [] { return Ask([] { return InGameplay(); }, 30s).value_or(false); });
    }

    // Totals of the VM catch-up counters over the trial, read once a second.
    struct CatchUpTotals
    {
        std::uint64_t frames{};
        std::uint64_t engaged{};
        std::uint64_t held{};
        std::uint64_t cycles{};
        std::uint64_t calls{};
        std::uint64_t violations{};
        std::uint64_t skips{};
        std::uint64_t rebuilds{};
        std::uint64_t gateCloses{};
    };

    void RecordCatchUp(CatchUpTotals& totals)
    {
        if (!config.catchUp) {
            return;
        }
        const auto stats = VMCatchUp::ReadAndResetStats();
        totals.frames += stats.frames;
        totals.engaged += stats.engagedFrames;
        totals.held += stats.heldFrames;
        totals.cycles += stats.cycles;
        totals.calls += stats.extraCalls;
        totals.violations += stats.guardViolations;
        totals.skips += stats.frontSkips;
        totals.rebuilds += stats.orderRebuilds;
        totals.gateCloses += stats.gateCloses;
        Record("cu", static_cast<std::uint32_t>(stats.frames), static_cast<std::uint32_t>(stats.cycles),
            std::format("frames={} engaged={} held={} cycles={} extra_calls={} leftover_tasklets={} blocked_skips={} blocked_stops={} "
                        "idle_stops={} direct_changes={} guard_violations={} overrun_pauses={} front_skips={} order_rebuilds={} gate_closes={}",
                stats.frames, stats.engagedFrames, stats.heldFrames, stats.cycles, stats.extraCalls, stats.leftoverTasklets, stats.blockedSkips,
                stats.blockedStops, stats.idleStops, stats.directChanges, stats.guardViolations, stats.overrunPauses, stats.frontSkips,
                stats.orderRebuilds, stats.gateCloses));
    }

    // Keeps the once-a-second catch-up rows, the scene state and the foreground share going while the
    // worker waits for something.
    struct Watch
    {
        CatchUpTotals    totals;
        Clock::time_point nextCatchUp{ Clock::now() + 1s };
        Clock::time_point nextPositions{ Clock::now() };
        bool             scenePlaying{};
        std::uint32_t    scenePhase{ 0xFFFFFFFF };
        bool             sceneSeenPlaying{};
        std::uint64_t    samples{};
        std::uint64_t    foreground{};

        void Tick()
        {
            const auto now = Clock::now();
            if (now >= nextCatchUp) {
                nextCatchUp = now + 1s;
                RecordCatchUp(totals);
            }
            if (now >= nextPositions) {
                nextPositions = now + 2s;
                SKSE::GetTaskInterface()->AddTask(RecordPositions);  // not waited for
            }
            // Plain reads of two fields of a form that is never freed; a torn read costs one sample.
            if (const auto* scene = scenePointer.load()) {
                const bool          playing = scene->isPlaying;
                const std::uint32_t phase = scene->currentPhaseIndex;
                if (playing != scenePlaying || phase != scenePhase) {
                    scenePlaying = playing;
                    scenePhase = phase;
                    sceneSeenPlaying = sceneSeenPlaying || playing;
                    Record("scene", riverwoodSceneID, phase, playing ? "playing" : "stopped");
                }
            }
            DWORD owner = 0;
            if (const HWND window = GetForegroundWindow()) {
                GetWindowThreadProcessId(window, &owner);
            }
            ++samples;
            foreground += owner == GetCurrentProcessId() ? 1 : 0;
        }

        [[nodiscard]] double ForegroundPct() const
        {
            return samples ? 100.0 * static_cast<double>(foreground) / static_cast<double>(samples) : 0.0;
        }

        template <class P>
        bool Until(std::stop_token stop, Clock::duration limit, P predicate)
        {
            const auto until = Clock::now() + limit;
            while (!stop.stop_requested()) {
                Tick();
                if (predicate()) {
                    return true;
                }
                if (Clock::now() >= until) {
                    return false;
                }
                std::this_thread::sleep_for(100ms);
            }
            return false;
        }

        bool For(std::stop_token stop, Clock::duration duration)
        {
            (void)Until(stop, duration, [] { return false; });
            return !stop.stop_requested();
        }
    };

    bool LoadSave(std::stop_token stop, const std::string& name)
    {
        const auto generation = loadGeneration.load();
        const auto failures = failedLoads.load();
        const auto requested = Ask([name] {
            auto* manager = RE::BGSSaveLoadManager::GetSingleton();
            if (!manager) {
                return false;
            }
            manager->PopulateSaveList();
            for (const auto* entry : manager->saveGameList) {
                if (entry && name == entry->fileName.c_str()) {
                    manager->Load(name.c_str());
                    return true;
                }
            }
            return false;
        });
        if (!requested.value_or(false)) {
            SKSE::log::error("quest probe: save {} is not in the active profile", name);
            return false;
        }
        if (!WaitUntil(stop, loadLimit, [&] { return loadGeneration.load() != generation || failedLoads.load() != failures; }) ||
            failedLoads.load() != failures) {
            SKSE::log::error("quest probe: loading {} failed or did not finish", name);
            return false;
        }
        return WaitGameplay(stop, loadLimit);
    }

    void AttachAnimSink(std::stop_token stop)
    {
        // The graph exists once Esbern's 3D is loaded.
        const bool attached = WaitUntil(stop, 15s, [] {
            return Ask([] {
                const World world = Resolve();
                if (!world.esbern || !world.esbern->Is3DLoaded()) {
                    return false;
                }
                world.esbern->RemoveAnimationGraphEventSink(&animSink);
                return world.esbern->AddAnimationGraphEventSink(&animSink);
            }).value_or(false);
        });
        Record("act", esbernID.load(), attached, attached ? "animation sink attached" : "animation sink not attached");
    }

    // Saves under this name and loads the save again in the same run, with a snapshot on each side.
    bool SaveAndReload(std::stop_token stop, Watch& watch, const std::string& name, const std::string& label)
    {
        (void)Ask([label] { return Snapshot((label + "_before_save").c_str()); });
        const auto saves = savesStarted.load();
        Record("act", 0, 0, "save " + name);
        if (!Ask([name] {
                auto* manager = RE::BGSSaveLoadManager::GetSingleton();
                if (manager) {
                    manager->Save(name.c_str());
                }
                return manager != nullptr;
            }).value_or(false) ||
            !watch.Until(stop, 60s, [&] { return savesStarted.load() != saves; })) {
            SKSE::log::error("quest probe: saving {} did not start", name);
            return false;
        }
        // The save message arrives while the file is still being written.
        if (!watch.For(stop, 4s)) {
            return false;
        }
        Record("act", 0, 0, "load " + name);
        if (!LoadSave(stop, name) || !watch.For(stop, 5s)) {
            return false;
        }
        AttachAnimSink(stop);
        return Ask([label] { return Snapshot((label + "_after_reload").c_str()); }).value_or(false);
    }

    struct Outcome
    {
        bool        ok{};
        std::string reason;
    };

    Outcome RunSegmentA(std::stop_token stop, unsigned trial, Watch& watch)
    {
        const auto start = Ask([] {
            const World world = Resolve();
            if (!world.Complete()) {
                return std::string("a quest, scene, actor, door or faction of the segment was not found");
            }
            (void)Snapshot("start");
            if (!StageDone(world.mq203, 10, world.player) || StageDone(world.mq203, 20, world.player) || StageDone(world.mq203, 30, world.player)) {
                return std::string("the start save is not at MQ203 stage 10 with 20 and 30 open");
            }
            return std::string();
        });
        if (!start) {
            return { false, "the game did not run the start check" };
        }
        if (!start->empty()) {
            return { false, *start };
        }
        const auto innCell = Ask([] {
            const World world = Resolve();
            const auto* cell = world.innDoor ? world.innDoor->GetParentCell() : nullptr;
            return cell ? cell->GetFormID() : RE::FormID{ 0 };
        }).value_or(0);
        if (innCell == 0) {
            return { false, "the inn door has no cell" };
        }

        // Riverwood: the player through the inn door's link to the street, then Esbern to the player.
        Record("act", innDoorID, 0, "player to Riverwood");
        if (!Ask([] { return SendPlayerThrough(Resolve().innDoor); }).value_or(false)) {
            return { false, "the inn door has no teleport link" };
        }
        if (!watch.For(stop, 1s) || !watch.Until(stop, arrivalLimit, [] {
                return Ask([] {
                    const auto* cell = RE::PlayerCharacter::GetSingleton()->GetParentCell();
                    return InGameplay() && cell && cell->IsExteriorCell();
                }, 30s).value_or(false);
            })) {
            return { false, "the player did not arrive in Riverwood" };
        }
        Record("act", esbernID.load(), 0, "Esbern to player (Riverwood)");
        (void)Ask([] {
            const World world = Resolve();
            world.esbern->MoveTo(world.player);
            return true;
        });
        if (!watch.Until(stop, stageTriggerLimit, [] { return Seen(20); })) {
            return { false, "stage 20 was not set after Esbern arrived in Riverwood" };
        }
        if (!watch.For(stop, 3s)) {
            return { false, "stopped" };
        }

        // The inn: the player through the street door's link, then Esbern to the player.
        Record("act", innDoorID, 1, "player into the inn");
        if (!Ask([] { return SendPlayerThrough(Resolve().outsideDoor); }).value_or(false)) {
            return { false, "the street door has no teleport link" };
        }
        if (!watch.For(stop, 1s) || !watch.Until(stop, arrivalLimit, [innCell] {
                return Ask([innCell] {
                    const auto* cell = RE::PlayerCharacter::GetSingleton()->GetParentCell();
                    return InGameplay() && cell && cell->GetFormID() == innCell;
                }, 30s).value_or(false);
            })) {
            return { false, "the player did not arrive in the inn" };
        }
        if (config.entryDelay > 0.0 &&
            !watch.For(stop, std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(config.entryDelay)))) {
            return { false, "stopped" };
        }
        Record("act", esbernID.load(), 1, "Esbern to player (inn)");
        (void)Ask([] {
            const World world = Resolve();
            world.esbern->MoveTo(world.player);
            return true;
        });
        AttachAnimSink(stop);
        if (!watch.Until(stop, stageTriggerLimit, [] { return Seen(30); })) {
            return { false, "stage 30 was not set after Esbern arrived in the inn" };
        }

        // From here the game runs the scene on its own. The scene does not end by itself: after the
        // phase that sets stage 60 it runs into phase 29, which lasts while MQ203 is below stage 70,
        // and only the player's dialogue with Delphine sets 70. The segment therefore ends when the
        // scene has reached that waiting phase (or has stopped) after stage 60.
        std::optional<Clock::time_point> sixty;
        const auto                       finished = [&] {
            if (!Seen(60)) {
                return false;
            }
            if (!sixty) {
                sixty = Clock::now();
            }
            return !watch.scenePlaying || watch.scenePhase >= waitingPhase || Clock::now() - *sixty > waitingPhaseLimit;
        };
        if (config.midSave) {
            // Stage 50 is set while the scene still has eleven phases to run. The save and the load
            // do not count against the scene's limit: the wait below starts over afterwards.
            if (watch.Until(stop, sceneLimit, [&] { return Seen(50) || finished(); }) && !finished() &&
                !SaveAndReload(stop, watch, std::format("APB_QP_A{}_mid", trial), "mid")) {
                (void)Ask([] { return Snapshot("end"); });
                return { false, "saving and reloading during the scene failed" };
            }
        }
        const bool ended = watch.Until(stop, sceneLimit, finished);
        // The fragments of stage 60 and the packages of the waiting phase get a moment to settle.
        if (ended && !watch.For(stop, 3s)) {
            return { false, "stopped" };
        }
        (void)Ask([] { return Snapshot("end"); });
        if (ended && watch.scenePlaying && watch.scenePhase < waitingPhase) {
            return { false, std::format("stage 60 was set but the scene stayed at phase {} for {} s",
                                static_cast<std::int32_t>(watch.scenePhase), waitingPhaseLimit.count()) };
        }
        if (!ended) {
            return { false, std::format("limit reached: stage40={} stage48={} stage50={} stage60={} scene_playing={} scene_phase={}",
                                Seen(40), Seen(48), Seen(50), Seen(60), watch.scenePlaying, static_cast<std::int32_t>(watch.scenePhase)) };
        }
        if (!watch.sceneSeenPlaying) {
            // The scene fields were never read as playing, so "scene ended" above only meant stage 60.
            return { true, "scene state was never read as playing; ended on stage 60 alone" };
        }
        return { true, "" };
    }

    void Run(std::stop_token stop)
    {
        // The first load of the start save is APBTestSaveLoader's.
        if (!WaitUntil(stop, 10min, [] { return loadGeneration.load() != 0 || failedLoads.load() != 0; }) || failedLoads.load() != 0 ||
            !WaitGameplay(stop, loadLimit)) {
            SKSE::log::error("quest probe aborted: the start save did not load");
            return;
        }
        unsigned passed = 0;
        for (unsigned trial = 1; trial <= config.trials && !stop.stop_requested(); ++trial) {
            if (trial > 1 && !LoadSave(stop, config.startSave)) {
                SKSE::log::error("quest probe aborted: trial={} could not reload the start save", trial);
                return;
            }
            // Past the load's own script burst, as the survey does.
            if (!Pause(stop, 5s)) {
                return;
            }
            for (auto& seen : stageSeen) {
                seen.store(false);
            }
            if (config.catchUp) {
                (void)VMCatchUp::ReadAndResetStats();
            }
            trialStart.store(Clock::now().time_since_epoch().count());
            recording.store(true, std::memory_order_release);
            Watch watch;
            auto  outcome = RunSegmentA(stop, trial, watch);
            const double sceneMS = TrialMS();
            if (outcome.ok) {
                // The end state must also survive a save and a load.
                if (!SaveAndReload(stop, watch, std::format("APB_QP_A{}_end", trial), "end")) {
                    outcome = { false, "saving and reloading after the segment failed" };
                }
            }
            RecordCatchUp(watch.totals);
            recording.store(false, std::memory_order_release);
            const auto dropped = FlushRows(trial);
            passed += outcome.ok ? 1 : 0;
            SKSE::log::info("quest probe trial={} segment=A result={} reason=\"{}\" scene_ms={:.0f} stage40={} stage48={} stage50={} stage60={} "
                            "cu_frames={} cu_engaged={} cu_held={} cu_cycles={} cu_extra_calls={} cu_front_skips={} cu_order_rebuilds={} cu_gate_closes={} cu_guard_violations={} foreground_pct={:.1f} dropped_rows={}",
                trial, outcome.ok ? "ok" : "failed", outcome.reason, sceneMS, Seen(40), Seen(48), Seen(50), Seen(60), watch.totals.frames,
                watch.totals.engaged, watch.totals.held, watch.totals.cycles, watch.totals.calls, watch.totals.skips, watch.totals.rebuilds,
                watch.totals.gateCloses, watch.totals.violations,
                watch.ForegroundPct(), dropped);
            if (stop.stop_requested()) {
                return;
            }
        }
        SKSE::log::info("quest probe complete trials={} ok={}", config.trials, passed);
    }

    std::jthread worker;

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        switch (message->type) {
        case SKSE::MessagingInterface::kPostPostLoad:
            if (config.catchUp && !InstallCatchUp()) {
                config.trials = 0;  // the run script sees the abort line; nothing is driven
            }
            break;
        case SKSE::MessagingInterface::kDataLoaded: {
            auto* events = RE::ScriptEventSourceHolder::GetSingleton();
            auto* ui = RE::UI::GetSingleton();
            if (!events || !ui) {
                SKSE::log::error("quest probe aborted: event sources unavailable");
                config.trials = 0;
                break;
            }
            events->AddEventSink(&stageSink);
            events->AddEventSink(&questSink);
            events->AddEventSink(&topicSink);
            events->AddEventSink(&equipSink);
            events->AddEventSink(&locationSink);
            ui->AddEventSink(&menuSink);
            if (config.trials != 0) {
                worker = std::jthread(Run);
            }
            break;
        }
        case SKSE::MessagingInterface::kPreLoadGame:
            gameplayReady.store(false);
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            if (message->data) {
                MenuWatch::Sync();
                if (config.catchUp) {
                    VMCatchUp::NotifyGameLoaded();
                }
                gameplayReady.store(true);
                ++loadGeneration;
            } else {
                ++failedLoads;
            }
            break;
        case SKSE::MessagingInterface::kSaveGame:
            ++savesStarted;
            break;
        default:
            break;
        }
    }
}

bool QuestSegmentProbe::Load(const SKSE::LoadInterface* skse)
{
    const auto env = [](const char* name) {
        const char* value = std::getenv(name);
        return std::string(value ? value : "");
    };
    config.catchUp = !env("APB_CATCHUP_ON").empty() && env("APB_CATCHUP_ON") != "none" && env("APB_CATCHUP_ON") != "0";
    SKSE::Init(skse, { .logName = "APBQuestProbe", .trampoline = config.catchUp, .trampolineSize = config.catchUp ? 256u : 0u });

    if (env("APB_QUEST_SEGMENT") != "A") {
        SKSE::log::error("quest probe aborted: APB_QUEST_SEGMENT must be A (the only segment implemented)");
        return true;
    }
    config.startSave = env("APB_TEST_SAVE");
    if (config.startSave.empty()) {
        SKSE::log::error("quest probe aborted: APB_TEST_SAVE names no start save");
        return true;
    }
    if (const auto trials = env("APB_QUEST_TRIALS"); !trials.empty()) {
        const auto count = std::strtoul(trials.c_str(), nullptr, 10);
        if (count < 1 || count > 20) {
            SKSE::log::error("quest probe aborted: APB_QUEST_TRIALS must be 1 to 20");
            return true;
        }
        config.trials = static_cast<unsigned>(count);
    }
    config.midSave = env("APB_QUEST_MIDSAVE") == "1";
    if (const auto delay = env("APB_QUEST_ENTRY_DELAY"); !delay.empty()) {
        config.entryDelay = std::strtod(delay.c_str(), nullptr);
        if (!(config.entryDelay >= 0.0 && config.entryDelay <= 60.0)) {
            SKSE::log::error("quest probe aborted: APB_QUEST_ENTRY_DELAY must be 0 to 60 seconds");
            return true;
        }
    }
    const auto directory = SKSE::log::log_directory();
    if (!directory) {
        SKSE::log::error("quest probe aborted: no SKSE log directory");
        return true;
    }
    rowFile = *directory / "APBQuestProbe.tsv";
    {
        std::ofstream file(rowFile, std::ios::trunc);
        file << "trial\tms\tframe\tthread\tkind\ta\tb\ttext\n";
    }
    rows.reserve(65536);
    // APB_CATCHUP_SKIP=1: the skip rule, which lets extra cycles pass calls that must wait
    // (VMCatchUp::SetSkipBlocked). In the release plugin it is the option SkipWaitingCalls, off by
    // default.
    const bool skipBlocked = config.catchUp && env("APB_CATCHUP_SKIP") == "1";
    // The mode InstallCatchUp will ask for; an unknown name is reported there.
    const auto catchUpMode = !config.catchUp ? std::string("none") : env("APB_CATCHUP_MODE").empty() ? std::string("accelerate") : env("APB_CATCHUP_MODE");
    if (skipBlocked && catchUpMode != "accelerate") {
        // The rule exists in accelerate only; a run labelled with it must not run another rule.
        SKSE::log::error("quest probe aborted: APB_CATCHUP_SKIP=1 needs APB_CATCHUP_MODE=accelerate, not '{}'", catchUpMode);
        return true;
    }
    VMCatchUp::SetSkipBlocked(skipBlocked);
    SKSE::log::info("quest probe segment=A trials={} mid_save={} catch_up={} catch_up_mode={} skip_blocked={} entry_delay_s={:.1f} start_save={}",
        config.trials, config.midSave, config.catchUp, catchUpMode, skipBlocked, config.entryDelay, config.startSave);
    return SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
}
