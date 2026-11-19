#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

// The menu part of the gameplay gate, without the game's types so that the unit tests cover it.
// MenuWatch.h applies it to the game's UI for the release plugin and the test plugins alike.
//
// The gate is closed while the loading or the main menu is open, or the game is paused.
//
// Whether the two menus are open is taken from their own open and close events. A menu event
// arrives on whichever thread runs the UI at the time, and SKSE runs its tasks on several threads
// as well (in one test trial menu events came on five threads and task rows on six). Which of
// them may read the UI's menu list while another changes it has not been established, so the
// list is read as rarely as possible: once after every load, to set what the events cannot tell
// (a sink added after a menu opened) and to recover from an event that was missed. Every other
// menu event (a HUD or dialogue menu, any menu that is not one of the two) leaves the gate alone. Closing it for those as well, as the builds up to 2026-11-10 did, made
// each such event restart the frame-rate average and drop the owed cycles of the catch-up and
// budget policies whenever a VM update fell between the event and the reading that followed it.
//
// The pause is not kept here: the gate reads the UI's count of pausing menus when it is asked.
class MenuLatch
{
public:
    // How many events of each menu had arrived when a reading of the UI began.
    struct Ticket
    {
        std::uint64_t loading;
        std::uint64_t main;
    };

    // Thread of a menu event: the loading menu opened or closed.
    void SetLoading(bool open)
    {
        const std::lock_guard guard(lock);
        ++seen.loading;
        loadingOpen = open;
        Update();
    }

    // Thread of a menu event: the main menu opened or closed.
    void SetMain(bool open)
    {
        const std::lock_guard guard(lock);
        ++seen.main;
        mainOpen = open;
        Update();
    }

    // Call before reading the UI and pass the result to Sync.
    [[nodiscard]] Ticket Ask() const
    {
        const std::lock_guard guard(lock);
        return seen;
    }

    // What a reading of the UI found. A menu whose event arrived after Ask keeps what the event
    // said: the event is the later news, and the reading may have been taken before it.
    void Sync(Ticket asked, bool loadingIsOpen, bool mainIsOpen)
    {
        const std::lock_guard guard(lock);
        if (asked.loading == seen.loading) {
            loadingOpen = loadingIsOpen;
        }
        if (asked.main == seen.main) {
            mainOpen = mainIsOpen;
        }
        synced = true;
        Update();
    }

    // Closed until the first Sync: events alone do not say what was open before the sink existed.
    [[nodiscard]] bool Blocked() const noexcept
    {
        return blocked.load(std::memory_order_acquire);
    }

private:
    void Update()
    {
        blocked.store(!synced || loadingOpen || mainOpen, std::memory_order_release);
    }

    mutable std::mutex lock;
    Ticket seen{};
    bool loadingOpen{};
    bool mainOpen{};
    bool synced{};
    std::atomic_bool blocked{ true };
};
