#pragma once

#include <array>
#include <cstddef>
#include <string_view>

// Used by the "accelerate" mode only. A native function counts as a read when its name starts with
// a read verb and contains no word that suggests a change; everything else, including a name that
// cannot be read, is assumed to change game state. A name is a heuristic, not proof: two reads can
// still observe different game states depending on whether a game update ran between them, which is
// why accelerate is an experimental mode.
namespace NativeClassificationDetail
{
    constexpr char Lower(char c)
    {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    inline bool HasUpper(std::string_view name)
    {
        for (const char c : name) {
            if (c >= 'A' && c <= 'Z') {
                return true;
            }
        }
        return false;
    }

    inline bool StartsWith(std::string_view name, std::string_view prefix)
    {
        if (name.size() <= prefix.size()) {
            return false;
        }
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            if (Lower(name[i]) != prefix[i]) {
                return false;
            }
        }
        return true;
    }

    inline bool Contains(std::string_view name, std::string_view word)
    {
        if (name.size() < word.size()) {
            return false;
        }
        for (std::size_t start = 0; start + word.size() <= name.size(); ++start) {
            bool match = true;
            for (std::size_t i = 0; i < word.size(); ++i) {
                if (Lower(name[start + i]) != word[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return true;
            }
        }
        return false;
    }
}

inline bool IsReadOnlyNativeName(std::string_view name)
{
    using namespace NativeClassificationDetail;
    constexpr std::array<std::string_view, 6> readVerbs{ "get", "is", "has", "find", "count", "worn" };
    // Conservative: a read name that mentions any of these words is treated as a change.
    // Words that commonly appear in read names (equip, play, lock, enable, move) are left out on
    // purpose: IsEquipped, GetPlayer, IsLocked and IsEnabled must stay reads.
    constexpr std::array<std::string_view, 15> changeWords{ "set", "add", "remove", "create", "delete", "reset",
        "clear", "push", "pop", "insert", "apply", "spawn", "increment", "decrement", "toggle" };
    bool read = false;
    for (const auto verb : readVerbs) {
        if (!StartsWith(name, verb)) {
            continue;
        }
        // The engine's string pool keeps the first spelling it saw. With mixed case, the next
        // character must start a new word ("GetPlayer", not "Getaway" or "Issue").
        const char next = name[verb.size()];
        if (HasUpper(name) && !(next >= 'A' && next <= 'Z') && !(next >= '0' && next <= '9')) {
            continue;
        }
        read = true;
        break;
    }
    if (!read) {
        return false;
    }
    for (const auto word : changeWords) {
        if (Contains(name, word)) {
            return false;
        }
    }
    return true;
}

namespace NativeClassificationDetail
{
    constexpr bool Equals(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (Lower(a[i]) != Lower(b[i])) {
                return false;
            }
        }
        return true;
    }
}

// Papyrus Tweaks NG has sped up natives since 2022 and leaves these script classes and name
// prefixes out of it by default (sScriptClassExclusions and the two "Get" entries of
// sScriptMethodPrefixesToExclude in PapyrusTweaks.ini 4.1.1). Its other prefixes are change verbs,
// which the read rule above rejects already. Its public history gives a symptom for the two MFG
// classes only (expressions playing too fast, 2.1) and "hoping for stability" for Quest (4.1.0);
// for the rest no reason is recorded. The protection is also not the same: there an excluded native
// is itself kept on the queued path, while here the excluded native still runs when its turn comes
// and only the stack's next queued call is kept out of the extra cycles. Taking the list over is
// therefore a precaution, not a proven requirement and not an equivalent of that plugin's rule.
inline bool IsHeldBackByPapyrusTweaks(std::string_view className, std::string_view name)
{
    using namespace NativeClassificationDetail;
    constexpr std::array<std::string_view, 10> classes{ "UI", "ConsoleUtil", "PO3_SKSEFunctions", "MfgConsole", "MFGConsoleFunc",
        "Input", "Debug", "Utility", "PapyrusTweaks", "Quest" };
    constexpr std::array<std::string_view, 2> prefixes{ "getanimation", "getquest" };
    for (const auto held : classes) {
        if (Equals(className, held)) {
            return true;
        }
    }
    for (const auto prefix : prefixes) {
        // A plain prefix as in Papyrus Tweaks: "GetQuest" itself is held back too.
        if (name.size() >= prefix.size() && Contains(name.substr(0, prefix.size()), prefix)) {
            return true;
        }
    }
    return false;
}

// Natives whose name reads like a query but whose implementation writes game data. Each entry is
// one miss of the name rule that was found by reading the native's source; the list is not a
// survey of third-party natives, and other such natives certainly exist.
//   WornObject.GetDisplayName (SKSE64, BaseExtraList::GetDisplayName): creates an
//     ExtraTextDisplayData on the item and adds it when the item has none and its health is not 1.0.
//   NiOverride.GetItemUniqueID / GetObjectUniqueID (RaceMenu skee64, ModifiedItem::GetAttributeData):
//     create ExtraRank and ExtraUniqueID on the item when it has none.
inline bool IsKnownChangeBehindReadName(std::string_view className, std::string_view name)
{
    using namespace NativeClassificationDetail;
    struct Entry
    {
        std::string_view className;
        std::string_view name;
    };
    constexpr std::array<Entry, 3> entries{ {
        { "WornObject", "GetDisplayName" },
        { "NiOverride", "GetItemUniqueID" },
        { "NiOverride", "GetObjectUniqueID" },
    } };
    for (const auto& entry : entries) {
        if (Equals(className, entry.className) && Equals(name, entry.name)) {
            return true;
        }
    }
    return false;
}

// The vanilla/SKSE helpers that scripts call directly from tasklets and that leave game state alone:
// arithmetic, strings and log output.
inline bool IsHelperNative(std::string_view className, std::string_view name)
{
    using namespace NativeClassificationDetail;
    if (className.empty() || name.empty()) {
        return false;
    }
    if (Equals(className, "Math") || Equals(className, "StringUtil")) {
        return true;
    }
    // Debug.Trace and TraceStack are callable from tasklets in the stock game (the Debug registration,
    // ID 55394 on AE 1.6.1170, sets that flag for them), and Papyrus Tweaks lets such natives through
    // before it looks at its exclusion list. Its exclusion of the Debug class therefore does not
    // cover them, and writing a log line changes nothing another script reads.
    //
    // Utility.RandomInt and RandomFloat used to be listed here as well. They are queued natives in
    // the stock game (the Utility registration, ID 56819, sets the flag for none of its functions;
    // po3 Tweaks has an option that does), so they follow the exclusion of the Utility class.
    return Equals(className, "Debug") && (Equals(name, "Trace") || Equals(name, "TraceStack"));
}

// Whether a native leaves game state alone: a read by name, or one of the helpers above.
// Everything else, including a native whose class or name is not known, counts as a change.
// papyrusTweaksList = false leaves the Papyrus Tweaks exclusion list out; only test builds pass it,
// to measure what the list costs.
inline bool IsSideEffectFreeNative(std::string_view className, std::string_view name, bool papyrusTweaksList = true)
{
    if (className.empty() || name.empty()) {
        return false;
    }
    if (IsHelperNative(className, name)) {
        return true;
    }
    if (IsKnownChangeBehindReadName(className, name) || (papyrusTweaksList && IsHeldBackByPapyrusTweaks(className, name))) {
        return false;
    }
    return IsReadOnlyNativeName(name);
}

// The release rule of the accelerate mode: a native counts as a read only when it is a helper above
// or one of the natives below, whose implementation was read and found not to change anything a
// script or the game can read back. Every other native is a change, whatever its name. The name
// rule above is kept for test builds that compare the two (VMCatchUp::SetReadRule).
//
// An entry stands for one implementation, not for a name. At runtime (VMCatchUp.cpp) a game native
// is accepted only when its function object was built by the game executable and its callback is
// the address of callbackID (Address Library, AE 1.6.1170); an SKSE native only when its function
// object and callback lie in skse64_1_6_1170.dll and SKSE is the reviewed version. A helper is
// accepted when its function object and its callback lie in the same of those two images. A native
// that another plugin registered under the same name is therefore a change.
//
// What was accepted as "no change" (verification/ReadList-Review.md has the evidence per entry):
// field reads and lookups; balanced reference counts; locks taken and released; the thread-local
// memory tag; the per-thread cache of the extra-data lookup (ID 12329), which no other thread or
// script can see; interning a returned string; the one-time construction of the default-object
// table, which the game has built long before a script runs; the VM's own error log line when an
// argument is None. Not accepted, and so not listed although the name reads like a query:
// Actor.GetActorValue, GetActorValuePercentage and GetBaseActorValue (refill value caches and store
// a missing base value in the actor), Actor.HasSpell (builds the actor's levelled spell list),
// ObjectReference.GetItemCount and IsInDialogueWithPlayer (create a reference handle),
// Keyword.GetKeyword (fills a cache shared by all threads), NetImmerse.HasNode and GetNodeScale
// (the name search of a flattened bone tree, ID 70743, creates the node of a bone that has none and
// attaches it to the skeleton), ObjectReference.GetDisplayName (ID 19781 creates an
// ExtraTextDisplayData on the reference, rewrites a cached name, or creates a reference handle).
//
// Reading an auto property is a read as well, but it is not in this table: the VM builds a new
// function object for every access, so it is recognised by its vtable (VMCatchUp.cpp).
enum class NativeOrigin : std::uint8_t
{
    kGame,  // registered by SkyrimSE.exe
    kSKSE,  // registered by skse64_1_6_1170.dll
};

struct ReviewedRead
{
    std::string_view className;
    std::string_view name;
    NativeOrigin origin;
    std::uint64_t callbackID;  // game natives: Address Library ID of the callback; SKSE natives: 0
};

// SKSE natives were read in the v2.2.8 sources (packed as SKSE reports its version).
inline constexpr std::uint32_t reviewedSKSEVersion = (2u << 24) | (2u << 16) | (8u << 4);

inline constexpr std::array<ReviewedRead, 44> reviewedReads{ {
    { "Actor", "GetEquippedArmorInSlot", NativeOrigin::kGame, 54684 },
    { "Actor", "GetEquippedWeapon", NativeOrigin::kGame, 54924 },
    { "Actor", "GetFactionRank", NativeOrigin::kGame, 54686 },
    { "Actor", "GetRace", NativeOrigin::kGame, 54930 },
    { "Actor", "HasMagicEffect", NativeOrigin::kGame, 54649 },
    { "Actor", "HasMagicEffectWithKeyword", NativeOrigin::kGame, 54650 },
    { "Actor", "HasPerk", NativeOrigin::kGame, 54644 },
    { "Actor", "IsDead", NativeOrigin::kGame, 54705 },
    { "Actor", "IsInCombat", NativeOrigin::kGame, 54945 },
    { "Actor", "IsInFaction", NativeOrigin::kGame, 54946 },
    { "Actor", "IsSneaking", NativeOrigin::kGame, 54953 },
    { "Actor", "WornHasKeyword", NativeOrigin::kGame, 54777 },
    { "ActorBase", "GetSex", NativeOrigin::kGame, 56005 },
    { "Alias", "GetOwningQuest", NativeOrigin::kGame, 55155 },
    { "Form", "GetFormID", NativeOrigin::kGame, 55902 },  // [NativePacing] lets tasklets run it; NativePacing.cpp takes the callback from here
    { "Form", "HasKeyword", NativeOrigin::kGame, 55870 },
    { "FormList", "Find", NativeOrigin::kGame, 55669 },
    { "FormList", "HasForm", NativeOrigin::kGame, 55682 },
    { "Game", "GetFormFromFile", NativeOrigin::kGame, 55465 },
    { "Game", "GetPlayer", NativeOrigin::kGame, 55469 },
    { "GlobalVariable", "GetValue", NativeOrigin::kGame, 55930 },
    { "ObjectReference", "GetBaseObject", NativeOrigin::kGame, 56168 },
    { "ObjectReference", "GetDistance", NativeOrigin::kGame, 56170 },
    { "ObjectReference", "GetPositionX", NativeOrigin::kGame, 56178 },
    { "ObjectReference", "GetPositionY", NativeOrigin::kGame, 56179 },
    { "ObjectReference", "GetPositionZ", NativeOrigin::kGame, 56180 },
    { "ObjectReference", "GetScale", NativeOrigin::kGame, 56633 },
    { "ObjectReference", "Is3DLoaded", NativeOrigin::kGame, 56188 },
    { "ObjectReference", "IsDisabled", NativeOrigin::kGame, 56639 },
    { "ReferenceAlias", "GetReference", NativeOrigin::kGame, 55287 },
    { "Actor", "GetEquippedObject", NativeOrigin::kSKSE, 0 },
    { "Actor", "GetWornForm", NativeOrigin::kSKSE, 0 },
    { "Armor", "GetMaskForSlot", NativeOrigin::kSKSE, 0 },
    { "Form", "GetName", NativeOrigin::kSKSE, 0 },
    { "Game", "GetLightModName", NativeOrigin::kSKSE, 0 },
    { "Game", "GetModByName", NativeOrigin::kSKSE, 0 },
    { "Game", "GetModName", NativeOrigin::kSKSE, 0 },
    { "MagicEffect", "GetCastingArt", NativeOrigin::kSKSE, 0 },
    { "MagicEffect", "GetHitEffectArt", NativeOrigin::kSKSE, 0 },
    { "MagicEffect", "GetLight", NativeOrigin::kSKSE, 0 },
    { "Shout", "GetNthSpell", NativeOrigin::kSKSE, 0 },
    { "Shout", "GetNthWordOfPower", NativeOrigin::kSKSE, 0 },
    { "SKSE", "GetVersionRelease", NativeOrigin::kSKSE, 0 },
    { "Spell", "GetNthEffectMagicEffect", NativeOrigin::kSKSE, 0 },
} };

// The entry for a class and function name, or nullptr. A name match alone does not make a native a
// read; see the runtime check in VMCatchUp.cpp.
constexpr const ReviewedRead* FindReviewedRead(std::string_view className, std::string_view name)
{
    using namespace NativeClassificationDetail;
    for (const auto& entry : reviewedReads) {
        if (Equals(className, entry.className) && Equals(name, entry.name)) {
            return &entry;
        }
    }
    return nullptr;
}
