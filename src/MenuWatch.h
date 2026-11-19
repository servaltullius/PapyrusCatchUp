#pragma once

#include "MenuGate.h"

// The menu part of the gameplay gate applied to the game's UI; MenuGate.h has the rules. One state
// per DLL. The release plugin, the quest probe and the gameplay trace all use this file, so that a
// test run closes and opens the gate exactly as the release plugin does.
namespace MenuWatch
{
    namespace Detail
    {
        inline MenuLatch latch;
    }

    // Reads from the UI whether the loading and the main menu are open. Call when a save has
    // finished loading or a new game starts (kPostLoadGame, kNewGame), on the thread of that
    // message (and as a fallback if OnEvent ever receives a null event). Reads the UI's menu list,
    // which has no lock here.
    inline void Sync()
    {
        const auto asked = Detail::latch.Ask();
        auto* ui = RE::UI::GetSingleton();
        // Without a UI nothing can be read; the gate stays closed until the next load.
        Detail::latch.Sync(asked, !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME),
            ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME));
    }

    // For a MenuOpenCloseEvent sink, on the thread the event arrived on. Reads the event alone.
    inline void OnEvent(const RE::MenuOpenCloseEvent* event)
    {
        if (!event) {
            Sync();  // nothing to go by; the UI is the only source left
        } else if (event->menuName == RE::LoadingMenu::MENU_NAME) {
            Detail::latch.SetLoading(event->opening);
        } else if (event->menuName == RE::MainMenu::MENU_NAME) {
            Detail::latch.SetMain(event->opening);
        }
    }

    // Whether the menus keep the gate closed. Any thread: besides the latch it reads one integer of
    // the UI, the count of open menus that pause the game.
    [[nodiscard]] inline bool Blocked()
    {
        if (Detail::latch.Blocked()) {
            return true;
        }
        auto* ui = RE::UI::GetSingleton();
        return !ui || ui->GameIsPaused();
    }
}
