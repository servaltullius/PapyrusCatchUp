#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Locates E8 relative call sites inside an engine function by resolved callee address. A site that
// comes with a prefix (the instructions before the call, ending in its 0xE8) must also carry that
// prefix: the hook relies on the registers and stack slots those instructions set up, and the
// prefix is what was checked against the disassembled SE 1.5.97 and AE 1.6.1170 executables
// (tests/verify_call_sites.py). The prefix also finds a call another plugin already redirected
// through SKSE's trampoline (for example Papyrus Tweaks NG), so it can be chained.
namespace CallSiteFinder
{
    inline constexpr std::int16_t kWildcard = -1;

    // The game executable's address range. A call that another plugin redirected through SKSE's
    // trampoline leads outside it; a call that leads to some other function of the game is not a
    // redirected stock call and is never taken for one.
    struct Bounds
    {
        std::uintptr_t begin{};
        std::uintptr_t end{};

        [[nodiscard]] constexpr bool Holds(std::uintptr_t address) const noexcept { return address >= begin && address < end; }
    };

    inline std::uintptr_t CallTarget(std::uintptr_t callAddress) noexcept
    {
        std::int32_t displacement{};
        std::memcpy(&displacement, reinterpret_cast<const void*>(callAddress + 1), sizeof(displacement));
        return callAddress + 5 + static_cast<std::intptr_t>(displacement);
    }

    inline std::uintptr_t CallTargetInBuffer(std::uintptr_t baseAddress, std::span<const std::uint8_t> code,
        std::size_t callOffset) noexcept
    {
        std::int32_t displacement{};
        std::memcpy(&displacement, code.data() + callOffset + 1, sizeof(displacement));
        return baseAddress + callOffset + 5 + static_cast<std::intptr_t>(displacement);
    }

    // Finds every 5-byte `E8 <rel32>` call in `code` whose decoded target equals `targetAddress`.
    inline std::vector<std::size_t> FindCallsTo(std::uintptr_t baseAddress, std::span<const std::uint8_t> code,
        std::uintptr_t targetAddress)
    {
        std::vector<std::size_t> hits;
        if (code.size() < 5 || targetAddress == 0) {
            return hits;
        }
        const std::size_t limit = code.size() - 5;
        for (std::size_t i = 0; i <= limit; ++i) {
            if (code[i] == 0xE8 && CallTargetInBuffer(baseAddress, code, i) == targetAddress) {
                hits.push_back(i);
            }
        }
        return hits;
    }

    // Finds every offset in `code` matching `pattern` (`kWildcard` matches any byte).
    inline std::vector<std::size_t> FindPattern(std::span<const std::uint8_t> code,
        std::span<const std::int16_t> pattern)
    {
        std::vector<std::size_t> hits;
        if (pattern.empty() || code.size() < pattern.size()) {
            return hits;
        }
        const std::size_t limit = code.size() - pattern.size();
        for (std::size_t i = 0; i <= limit; ++i) {
            bool match = true;
            for (std::size_t j = 0; j < pattern.size(); ++j) {
                if (pattern[j] != kWildcard && code[i + j] != static_cast<std::uint8_t>(pattern[j])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                hits.push_back(i);
            }
        }
        return hits;
    }

    inline bool HasPrefixAt(std::span<const std::uint8_t> code, std::size_t callOffset,
        std::span<const std::int16_t> prefixEndingInE8) noexcept
    {
        if (prefixEndingInE8.empty() || callOffset + 1 < prefixEndingInE8.size() || callOffset + 5 > code.size()) {
            return false;
        }
        const std::size_t start = callOffset + 1 - prefixEndingInE8.size();
        for (std::size_t j = 0; j < prefixEndingInE8.size(); ++j) {
            if (prefixEndingInE8[j] != kWildcard &&
                code[start + j] != static_cast<std::uint8_t>(prefixEndingInE8[j])) {
                return false;
            }
        }
        return true;
    }

    struct Match
    {
        std::size_t callOffset{};
        bool chained{};
    };

    // Locates a unique `E8 <rel32>` call to `targetAddress` inside `code`.
    // - Without a prefix: exactly one call to `targetAddress`, else nothing.
    // - With a prefix: exactly one call to `targetAddress` that carries the prefix; a call without
    //   it is not accepted even when it is the only one (`chained = false`).
    // - With a prefix and no call to `targetAddress`: a unique match of the prefix (which must end
    //   with 0xE8) whose call leads outside `image`, a site another plugin already hooked
    //   (`chained = true`).
    inline std::optional<Match> FindUniqueCall(std::uintptr_t baseAddress, std::span<const std::uint8_t> code,
        std::uintptr_t targetAddress, std::span<const std::int16_t> prefixEndingInE8, Bounds image)
    {
        const auto hits = FindCallsTo(baseAddress, code, targetAddress);
        if (prefixEndingInE8.empty()) {
            return hits.size() == 1 ? std::optional<Match>(Match{ hits[0], false }) : std::nullopt;
        }
        if (!hits.empty()) {
            std::vector<std::size_t> filtered;
            for (const auto offset : hits) {
                if (HasPrefixAt(code, offset, prefixEndingInE8)) {
                    filtered.push_back(offset);
                }
            }
            if (filtered.size() == 1) {
                return Match{ filtered[0], false };
            }
            return std::nullopt;
        }
        if (hits.empty() && !prefixEndingInE8.empty() && prefixEndingInE8.back() == 0xE8) {
            const auto patternHits = FindPattern(code, prefixEndingInE8);
            if (patternHits.size() == 1) {
                const std::size_t callOffset = patternHits[0] + prefixEndingInE8.size() - 1;
                if (callOffset + 5 <= code.size() && !image.Holds(CallTargetInBuffer(baseAddress, code, callOffset))) {
                    return Match{ callOffset, true };
                }
            }
        }
        return std::nullopt;
    }

    struct MultiMatch
    {
        std::vector<std::size_t> callOffsets;
        bool chained{};
    };

    // Locates exactly `expectedCount` `E8 <rel32>` calls to `targetAddress`, each carrying
    // `prefixEndingInE8` when one is given. When another plugin already hooked some of them, the
    // prefix must match exactly `expectedCount` calls: every remaining stock call among them, and the
    // others leading outside `image`.
    inline std::optional<MultiMatch> FindExactCalls(std::uintptr_t baseAddress, std::span<const std::uint8_t> code,
        std::uintptr_t targetAddress, std::size_t expectedCount, std::span<const std::int16_t> prefixEndingInE8, Bounds image)
    {
        if (expectedCount == 0) {
            return std::nullopt;
        }
        const auto hits = FindCallsTo(baseAddress, code, targetAddress);
        if (hits.size() == expectedCount) {
            const bool prefixed = prefixEndingInE8.empty() || std::ranges::all_of(hits, [&](std::size_t offset) {
                return HasPrefixAt(code, offset, prefixEndingInE8);
            });
            return prefixed ? std::optional<MultiMatch>(MultiMatch{ hits, false }) : std::nullopt;
        }
        if (hits.size() < expectedCount && !prefixEndingInE8.empty() && prefixEndingInE8.back() == 0xE8) {
            const auto patternHits = FindPattern(code, prefixEndingInE8);
            if (patternHits.size() == expectedCount) {
                std::vector<std::size_t> offsets;
                offsets.reserve(expectedCount);
                for (const auto pos : patternHits) {
                    const std::size_t callOffset = pos + prefixEndingInE8.size() - 1;
                    if (callOffset + 5 > code.size()) {
                        return std::nullopt;
                    }
                    const bool stock = std::ranges::find(hits, callOffset) != hits.end();
                    if (!stock && image.Holds(CallTargetInBuffer(baseAddress, code, callOffset))) {
                        return std::nullopt;
                    }
                    offsets.push_back(callOffset);
                }
                const bool allStockKept = std::ranges::all_of(hits, [&](std::size_t offset) {
                    return std::ranges::find(offsets, offset) != offsets.end();
                });
                if (!allStockKept) {
                    return std::nullopt;
                }
                return MultiMatch{ std::move(offsets), true };
            }
        }
        return std::nullopt;
    }
}
