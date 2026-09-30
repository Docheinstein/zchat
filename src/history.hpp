#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace zchat::history {

// The last chat messages (sent and received), kept in the config folder, so they can be shown again when zchat
// starts. Only messages: not pictures, joins or notices.
struct Entry {
    std::time_t time = 0;
    std::uint64_t sender = 0;
    std::string name;
    std::string text;
};

// How many messages are kept.
inline constexpr std::size_t max_entries = 50;

// Remembers a message, forgetting the oldest one past max_entries. Thread-safe. Failing to save is not an error:
// the history is a convenience.
void add(Entry entry);

// The messages remembered, oldest first.
std::vector<Entry> load();

} // namespace zchat::history
