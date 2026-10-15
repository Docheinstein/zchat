#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace zchat::store {

// What was said, kept by every zchat, so that whoever was not there when it was said gets it later: there is no
// server, so the zchats tell each other what they have (see Chat's sync), and each keeps all it can see, for whoever
// comes next, even when its author is gone.
//
// A record is something said in a topic: a channel (by name: "general", a channel, a private chat), or a game's
// ("~" and its name, e.g. "~wordle"), for what a game must remember even if nobody was there, like who played the
// Wordle of a day. Records never change: they are added, and forgotten once older than max_age.

struct Record {
    std::string topic;
    // Unique: for what is said, see key_of(); a game makes its own.
    std::string key;
    // When it was said (as the first zchat that kept it saw it).
    std::time_t time = 0;
    // Who: their sender id then (whose color it gives), user id (0 when unknown) and name.
    std::uint64_t sender = 0;
    std::uint64_t user = 0;
    std::string name;
    // 'm' a message, 'a' a drawing (see PacketType::Art), 'g' a game's record.
    char kind = 'm';
    std::string data;
    // Kept here only, never told about: the history of older versions, whose messages have no key the others know.
    bool local = false;
};

// How long records are kept, and told about.
inline constexpr std::time_t max_age = 30 * 24 * 60 * 60;
// The most records kept in a topic: the oldest go first.
inline constexpr std::size_t max_per_topic = 3000;
// The longest key.
inline constexpr std::size_t max_key = 80;

// The key of what a sender said, in a packet of theirs with that sequence number: the same for every zchat that got
// it. (The text is in it too, as a sender id and sequence numbers can come back in another session.)
std::string key_of(std::uint64_t sender, std::uint64_t seq, std::string_view data);

// Whether a key is one: up to max_key letters, digits, '-', '.', '_' and ':'.
bool valid_key(std::string_view key);
// Whether a topic is one: a channel's name, or "~" and a game's.
bool valid_topic(std::string_view topic);

// A record on one line (JSON), as kept and as sent; and back, nullopt when it is not one.
std::string encode(const Record& record);
std::optional<Record> decode(std::string_view line);

// The records, kept in a file (one per line), or only in memory without one. Thread-safe.
class Store {
public:
    // Loads the file, if any, forgetting what is too old (now is the time to compare with).
    explicit Store(std::filesystem::path file = {}, std::time_t now = std::time(nullptr));

    // Keeps a record; false when it is known already (by key), or too old.
    bool add(Record record, std::time_t now = std::time(nullptr));
    bool has(std::string_view key) const;
    // Changes with every record added: to tell whether anything is new.
    std::uint64_t version() const;

    // A topic's records, the oldest first.
    std::vector<Record> records(std::string_view topic) const;
    // The topics with records.
    std::vector<std::string> topics() const;

    // What tells whether two zchats have the same records of a topic, the ones they tell about (not local, not older
    // than max_age): a hash of their keys, 0 for none.
    std::uint32_t digest(std::string_view topic, std::time_t now = std::time(nullptr)) const;
    // The keys of a topic's records, as told to who has them (a day older than told about too, so that a record
    // about to be too old is not sent back and forth between clocks that do not agree).
    std::vector<std::string> keys(std::string_view topic, std::time_t now = std::time(nullptr)) const;
    // The records of a topic that someone with these keys does not have, the oldest first, up to max_bytes of them
    // encoded.
    std::vector<Record> missing(std::string_view topic, const std::set<std::string, std::less<>>& theirs,
                                std::size_t max_bytes, std::time_t now = std::time(nullptr)) const;

private:
    void forget_old_locked(std::time_t now);
    void save_all_locked() const;

    std::filesystem::path file_;
    mutable std::mutex mutex_;
    // By topic, then by key.
    std::map<std::string, std::map<std::string, Record, std::less<>>, std::less<>> topics_;
    std::set<std::string, std::less<>> keys_;
    std::uint64_t version_ = 0;
    std::size_t appended_ = 0;
};

// Updates that must happen at a time even if nobody is online then, like the end of a day of a game: kept in a file
// until they are done, by key, they are due once their time has come, here or the next time zchat starts. Thread-safe.
class Queue {
public:
    explicit Queue(std::filesystem::path file = {});

    // Adds an update due at due, unless one with the same key is waiting.
    void add(std::string key, std::time_t due);
    // Takes out the updates due by now whose keys start with prefix, the earliest first.
    std::vector<std::string> take_due(std::string_view prefix, std::time_t now = std::time(nullptr));
    bool waiting(std::string_view key) const;

private:
    void save_locked() const;

    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::map<std::string, std::time_t, std::less<>> due_;
};

} // namespace zchat::store
