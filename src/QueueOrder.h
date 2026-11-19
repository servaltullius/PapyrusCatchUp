#pragma once

#include <cstddef>
#include <span>
#include <unordered_map>
#include <vector>

// The order the native-call queue must have after an extra cycle that moved waiting calls behind
// the others (the skip rule, VMCatchUp::SetSkipBlocked). Kept apart from the hooks so that it can be
// checked without the game.
namespace QueueOrder
{
    // `queued`: the calls as they stand in the queue after the cycle, by key. `moved`: the calls the
    // cycle moved behind, in the order it moved them, which is the order they had among themselves.
    // Returns indices into `queued`: first the moved calls in that order, then every other call as
    // it stands. The others are calls the cycle never reached and calls that were queued while it
    // ran; both stood behind the moved calls before the cycle or did not exist yet, so the result is
    // the queue as it was, without the calls that ran and with the new calls at the end.
    // A moved call that is not in `queued` is left out; a key that occurs twice keeps its place.
    template <class Key>
    [[nodiscard]] std::vector<std::size_t> Restored(std::span<const Key> queued, std::span<const Key> moved)
    {
        std::unordered_map<Key, std::size_t> place;
        place.reserve(queued.size());
        for (std::size_t i = 0; i < queued.size(); ++i) {
            place.try_emplace(queued[i], i);  // the first one with this key
        }
        std::vector<bool> taken(queued.size(), false);
        std::vector<std::size_t> order;
        order.reserve(queued.size());
        for (const auto& key : moved) {
            if (const auto found = place.find(key); found != place.end() && !taken[found->second]) {
                taken[found->second] = true;
                order.push_back(found->second);
            }
        }
        for (std::size_t i = 0; i < queued.size(); ++i) {
            if (!taken[i]) {
                order.push_back(i);
            }
        }
        return order;
    }
}
