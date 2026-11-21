#include "NativePacing.h"

#include "NativeClassification.h"

#include <limits>

namespace
{
#ifdef APB_GAMEPLAY_CANDIDATE
    // AE 1.6.1170: Game.GetPlayer loads PlayerCharacter::singleton.
    constexpr std::array<std::uint8_t, 8> callbackCode{ 0x48, 0x8B, 0x05, 0x41, 0xA9, 0x77, 0x02, 0xC3 };
    constexpr auto vtableOffset = 0x1921570;
    constexpr auto className = "Game";
    constexpr auto functionName = "getplayer";
#else
    // AE 1.6.1170: Form.GetFormID's native callback only reads TESForm::formID.
    constexpr std::array<std::uint8_t, 5> callbackCode{ 0x41, 0x8B, 0x40, 0x14, 0xC3 };
    constexpr auto vtableOffset = 0x1925B40;
    constexpr auto className = "Form";
    constexpr auto functionName = "GetFormID";
#endif
    // The native that tasklets are let run must be one VM catch-up counts as a read: accelerate marks
    // a script as changed for every other native it calls directly, so each call let through here
    // would hold its script back to the next frame. The callback address is therefore taken from
    // the reviewed read list, and a target that is not on that list does not compile.
    constexpr const ReviewedRead* reviewedTarget = FindReviewedRead(className, functionName);
    static_assert(reviewedTarget && reviewedTarget->origin == NativeOrigin::kGame,
        "the native that NativePacing lets tasklets run must be a reviewed read of the game");
    constexpr std::array<std::uint8_t, 13> eligibilityCode{
        0x48, 0x8B, 0x01, 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x75, 0x13
    };
    constexpr std::array<std::uint8_t, 5> attemptCall{ 0xE8, 0xEC, 0xB4, 0xFE, 0xFF };

    std::uintptr_t stockCallback{};
    std::uintptr_t stockVtable{};
    std::atomic_uint remaining{};

#ifdef APB_REAL_TELEMETRY
    std::atomic_uint candidates{};
    std::atomic_uint promoted{};
    std::atomic_uint exhausted{};
    std::atomic_uint peak{};
#endif

    bool IsStockNative(const RE::BSScript::IFunction* function)
    {
        // Check the concrete engine marshaller before reading its native callback at +0x50.
        std::uintptr_t vtable{}, callback{};
        std::memcpy(&vtable, function, sizeof(vtable));
        if (vtable != stockVtable) {
            return false;
        }
        std::memcpy(&callback, reinterpret_cast<const std::byte*>(function) + 0x50, sizeof(callback));
        return callback == stockCallback && function->GetName() == functionName &&
            function->GetObjectTypeName() == className &&
            std::memcmp(reinterpret_cast<const void*>(callback), callbackCode.data(), callbackCode.size()) == 0;
    }

    bool CallableFromTasklet(RE::BSScript::IFunction* function)
    {
        if (function->CanBeCalledFromTasklets()) {
            return true;
        }
        auto tickets = remaining.load(std::memory_order_relaxed);
#ifndef APB_REAL_TELEMETRY
        if (tickets == 0) {
            return false;
        }
#endif
        if (!IsStockNative(function)) {
            return false;
        }
#ifdef APB_REAL_TELEMETRY
        ++candidates;
#endif
        while (tickets != 0) {
            if (remaining.compare_exchange_weak(tickets, tickets - 1, std::memory_order_relaxed)) {
#ifdef APB_REAL_TELEMETRY
                ++promoted;
                const auto used = NativePacing::callsPerUpdate - tickets + 1;
                auto previous = peak.load(std::memory_order_relaxed);
                while (previous < used && !peak.compare_exchange_weak(previous, used, std::memory_order_relaxed)) {}
#endif
                return true;
            }
        }
#ifdef APB_REAL_TELEMETRY
        ++exhausted;
#endif
        return false;
    }
}

bool NativePacing::Install()
{
    const auto base = REL::Module::get().base();
    stockCallback = REL::ID(reviewedTarget->aeCallbackID).address();
    stockVtable = base + vtableOffset;
    const auto eligibility = REL::ID(104853).address() + 0x61D;
    const auto attempt = REL::ID(105176).address() + 0x82F;
    if (!REL::verify_code(eligibility, eligibilityCode.data(), eligibilityCode.size()) ||
        !REL::verify_code(attempt, attemptCall.data(), attemptCall.size()) ||
        !REL::verify_code(stockCallback, callbackCode.data(), callbackCode.size())) {
        SKSE::log::warn("Native pacing disabled because the verified engine instructions differ");
        return false;
    }

    // The existing call site already has Win64 shadow space, and RCX is the IFunction.
    // Return to the original test/jump so a denied ticket takes the engine's normal queue path.
    std::array<std::uint8_t, 14> jump{ 0xFF, 0x25, 0, 0, 0, 0 };
    const auto target = reinterpret_cast<std::uintptr_t>(CallableFromTasklet);
    std::memcpy(jump.data() + 6, &target, sizeof(target));
    auto* stub = SKSE::GetTrampoline().allocate(jump.size());
    std::memcpy(stub, jump.data(), jump.size());
    const auto distance = reinterpret_cast<std::intptr_t>(stub) - static_cast<std::intptr_t>(eligibility + 5);
    if (distance < std::numeric_limits<std::int32_t>::min() || distance > std::numeric_limits<std::int32_t>::max()) {
        SKSE::log::warn("Native pacing trampoline is outside the call site's range");
        return false;
    }
    std::array<std::uint8_t, eligibilityCode.size()> patch{
        0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x84, 0xC0, 0x75, 0x13
    };
    const auto displacement = static_cast<std::int32_t>(distance);
    std::memcpy(patch.data() + 1, &displacement, sizeof(displacement));
    if (!REL::safe_write(eligibility, patch.data(), patch.size(), eligibilityCode)) {
        SKSE::log::warn("Native pacing eligibility call changed during installation");
        return false;
    }
    SKSE::log::info("Installed native pacing for stock {}.{}, limit={} per gameplay VM update",
        className, functionName, callsPerUpdate);
    return true;
}

void NativePacing::BeginUpdate(bool allowPromotion)
{
    remaining.store(allowPromotion ? callsPerUpdate : 0, std::memory_order_relaxed);
}

void NativePacing::Pause()
{
    remaining.store(0, std::memory_order_relaxed);
}

#ifdef APB_REAL_TELEMETRY
void NativePacing::ResetCounts()
{
    candidates = promoted = exhausted = peak = 0;
}

NativePacing::Counts NativePacing::ReadCounts()
{
    return { candidates.load(), promoted.load(), exhausted.load(), peak.load() };
}
#endif
