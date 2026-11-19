#include <cstdlib>

namespace
{
    constexpr auto runtime = SKSE::RUNTIME_SSE_1_6_1170;
    std::string saveName;

    std::int32_t WorkSlice(RE::StaticFunctionTag*)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(65);
        while (std::chrono::steady_clock::now() < end) {
            _mm_pause();
        }
        return 1;
    }

    bool RegisterStressWork(RE::BSScript::IVirtualMachine* vm)
    {
        vm->RegisterFunction("WorkSlice", "APBStressWork", WorkSlice, false);
        return true;
    }

    class StartupSpinner final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>* source) override
        {
            if (!event || !event->opening || event->menuName != RE::LoadWaitSpinner::MENU_NAME) {
                return RE::BSEventNotifyControl::kContinue;
            }
            source->RemoveEventSink(this);
            SKSE::GetTaskInterface()->AddTask([] {
                auto* manager = RE::BGSSaveLoadManager::GetSingleton();
                if (manager) {
                    manager->PopulateSaveList();
                    for (auto* entry : manager->saveGameList) {
                        if (entry && saveName == entry->fileName.c_str()) {
                            SKSE::log::info("Loading the requested benchmark save");
                            REL::Relocation<void (*)()>{ REL::ID(36604) }();
                            manager->Load(saveName.c_str());
                            return;
                        }
                    }
                }
                SKSE::log::warn("Exact benchmark save not present in the active profile");
            });
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    StartupSpinner startup;

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        if (message->type == SKSE::MessagingInterface::kInputLoaded) {
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->AddEventSink(&startup);
            }
        }
    }
}

SKSE_PLUGIN_VERSION = [] {
    SKSE::PluginVersionData info{};
    info.PluginName("APBTestSaveLoader");
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
    SKSE::Init(skse, { .logName = "APBTestSaveLoader" });
    const char* requestedSave = std::getenv("APB_TEST_SAVE");
    if (requestedSave && requestedSave[0] != '\0') {
        saveName = requestedSave;
        const char* stress = std::getenv("APB_STRESS");
        if (stress && std::string_view(stress) == "1" &&
            !SKSE::GetPapyrusInterface()->Register(RegisterStressWork)) {
            return false;
        }
        return SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    }
    return true;
}
