#pragma once

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

// The text of the log line that names what ran in a frame whose extra cycles took too long.
// Kept apart from the hooks so that the unit tests cover it.
namespace SlowFrameReport
{
    // "A.f x3, B.g, C.h (+2 more)": each name once with how often it occurred, the most frequent
    // first and otherwise in the order they first occurred, at most `cap` names. Empty names count
    // as "(unknown)".
    [[nodiscard]] inline std::string Summarize(const std::vector<std::string>& names, std::size_t cap)
    {
        struct Entry
        {
            std::string name;
            std::size_t count;
            std::size_t first;
        };
        std::vector<Entry> entries;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const std::string& name = names[i].empty() ? std::string("(unknown)") : names[i];
            const auto found = std::ranges::find(entries, name, &Entry::name);
            if (found != entries.end()) {
                ++found->count;
            } else {
                entries.push_back({ name, 1, i });
            }
        }
        std::ranges::stable_sort(entries, [](const Entry& a, const Entry& b) { return a.count > b.count; });
        std::string text;
        const std::size_t shown = std::min(cap, entries.size());
        for (std::size_t i = 0; i < shown; ++i) {
            if (!text.empty()) {
                text += ", ";
            }
            text += entries[i].name;
            if (entries[i].count > 1) {
                text += std::format(" x{}", entries[i].count);
            }
        }
        if (entries.size() > shown) {
            text += std::format(" (+{} more)", entries.size() - shown);
        }
        return text.empty() ? std::string("none") : text;
    }
}
