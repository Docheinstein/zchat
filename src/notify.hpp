#pragma once

#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zchat::notify {

// What notifies, and how, as in Discord: for the messages of the channels chosen (or only for the tags) and for the
// games and other events, a little window in a corner of the screen (see popup::toast()), a sound, the taskbar
// flashing; and Do Not Disturb, which keeps them all quiet for a while. Set with /notify (and /dnd), or with 🔔 in the
// window; kept in the config file.

// Which messages of a channel notify.
enum class Level {
    // The level of all the channels (Settings::level): only for a channel's own.
    Default,
    All,
    // Tags (@NAME, @everyone, a quote of ours) only; in a private chat, every message is one.
    Mentions,
    Nothing,
};

// What a desktop notification shows of a message.
enum class Preview {
    // Who sent it, where, and what it says.
    Full,
    // Who sent it, and where.
    Name,
    // Only that there is a new message.
    Hidden,
};

// The sounds of the window; a terminal has its bell only.
inline constexpr std::string_view sounds[] = {"ding", "chime", "pop", "blip", "bell", "knock"};

struct Settings {
    // The channels'. Private chats notify every message, unless a level of their own says otherwise.
    Level level = Level::Mentions;
    // A level for some channels (by name, or "@NAME" for a private chat), instead of the one of all.
    std::map<std::string, Level, std::less<>> channels;
    // Desktop notifications (popup::toast()) at all; for messages, and for games and other events.
    bool desktop = true;
    bool messages = true;
    bool events = true;
    // ... even when zchat is in front (otherwise only the sound, and only for elsewhere than the channel shown).
    bool when_focused = false;
    Preview preview = Preview::Full;
    bool sound = true;
    std::string sound_name = "ding";
    // 0 to 100.
    int volume = 60;
    // The taskbar flashes, until zchat is looked at.
    bool flash = true;
    // The number of unread messages in the title of the window.
    bool badge = true;
    // ... and on its button in the taskbar (see popup::badge()).
    bool taskbar = true;
    // @everyone does not notify (as if it tagged someone else).
    bool suppress_everyone = false;
    // Until when nothing notifies: 0 for never, dnd_forever until it is turned off.
    std::time_t dnd_until = 0;
    // In Do Not Disturb, trills (/trill) are not played either: they only show in the chat.
    bool dnd_trills = true;
};

inline constexpr std::time_t dnd_forever = static_cast<std::time_t>(-1);

// Whether Do Not Disturb is on at now.
bool dnd(const Settings& settings, std::time_t now);

// The level of a channel: its own, or else the one of all (Mentions-like All for private chats).
Level level_of(const Settings& settings, std::string_view channel, bool direct);

// What a new message calls for.
struct Message {
    std::string_view channel;
    // A private chat.
    bool direct = false;
    // It tags us; and whether only by @everyone.
    bool tags_us = false;
    bool only_everyone = false;
    // Ours (sent from here): never notifies.
    bool own = false;
    // In the channel shown.
    bool current = false;
    // zchat is in front (nullopt: it cannot be told, as in some terminals, and then it is taken as not).
    std::optional<bool> focused;
};

struct Decision {
    bool sound = false;
    bool desktop = false;
    bool flash = false;
    // Counted in the badge: false only when the channel's level is Nothing, or it is ours.
    bool counts = false;
};

Decision decide(const Settings& settings, const Message& message, std::time_t now);

// What an event (a game that starts, a vote…) calls for: a desktop notification and the taskbar flashing, unless in
// Do Not Disturb. Its sound is the game's own.
Decision decide_event(const Settings& settings, std::optional<bool> focused, std::time_t now);

// What a desktop notification of a message shows, as the preview allows: where it is from, who sent it (as given,
// e.g. colored), what it says (e.g. "sent a picture"), and whether to show who (their avatar).
struct Shown {
    std::string where;
    std::string title;
    std::string text;
    bool who = true;
};
Shown text(const Settings& settings, std::string_view who, std::string_view where, bool direct, std::string_view what);

// The names of the levels and previews, as /notify and the config file write them, and back.
std::string_view name(Level level);
std::string_view name(Preview preview);
std::optional<Level> parse_level(std::string_view text);
std::optional<Preview> parse_preview(std::string_view text);

// A duration as /dnd takes it: "30m", "1h", "8h", "2d", or a number of minutes. nullopt when it is none.
std::optional<std::time_t> parse_duration(std::string_view text);

// To and from the config file: its keys and values (all starting with "notify_"), read through get.
std::vector<std::pair<std::string, std::string>> to_config(const Settings& settings);
Settings from_config(const std::function<std::optional<std::string>(std::string_view key)>& get);

// To and from the window: JSON, as the 🔔 settings read and write them.
std::string to_json(const Settings& settings);
// Reads what the window sends over base: what it does not say stays as it is.
Settings from_json(std::string_view json, Settings base);

// The settings in use: loaded from the config file the first time, and saved there by set(). Thread-safe.
Settings current();
void set(Settings settings);

} // namespace zchat::notify
