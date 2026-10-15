#include "notify.hpp"

#include "config.hpp"
#include "json.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <mutex>

namespace zchat::notify {

namespace {

    constexpr std::string_view channel_prefix = "notify_channel_";

    std::string_view yes_no(bool value) {
        return value ? "yes" : "no";
    }

    std::string_view on_off(bool value) {
        return value ? "on" : "off";
    }

    std::optional<bool> parse_bool(std::string_view text) {
        if (text == "yes" || text == "on" || text == "true" || text == "1") {
            return true;
        }
        if (text == "no" || text == "off" || text == "false" || text == "0") {
            return false;
        }
        return std::nullopt;
    }

    template <typename T>
    std::optional<T> parse_number(std::string_view text) {
        T value {};
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc() || end != text.data() + text.size()) {
            return std::nullopt;
        }
        return value;
    }

    bool known_sound(std::string_view sound) {
        return std::ranges::find(sounds, sound) != std::end(sounds);
    }

    std::mutex mutex;
    std::optional<Settings> loaded;

} // namespace

bool dnd(const Settings& settings, std::time_t now) {
    return settings.dnd_until == dnd_forever || (settings.dnd_until != 0 && now < settings.dnd_until);
}

Level level_of(const Settings& settings, std::string_view channel, bool direct) {
    if (const auto it = settings.channels.find(channel); it != settings.channels.end() && it->second != Level::Default) {
        return it->second;
    }
    // Discord's way: a private message always notifies, unless that chat says otherwise.
    if (direct) {
        return settings.level == Level::Nothing ? Level::Nothing : Level::All;
    }
    return settings.level == Level::Default ? Level::Mentions : settings.level;
}

Decision decide(const Settings& settings, const Message& message, std::time_t now) {
    Decision decision;
    if (message.own) {
        return decision;
    }
    const Level level = level_of(settings, message.channel, message.direct);
    if (level == Level::Nothing) {
        return decision;
    }
    decision.counts = true;
    const bool tagged = message.tags_us && !(message.only_everyone && settings.suppress_everyone);
    if (level == Level::Mentions && !tagged && !message.direct) {
        return decision;
    }
    if (dnd(settings, now)) {
        return decision;
    }
    const bool focused = message.focused.value_or(false);
    // What is in front of us needs no telling.
    if (focused && message.current) {
        return decision;
    }
    decision.sound = settings.sound;
    decision.desktop = settings.desktop && settings.messages && (!focused || settings.when_focused);
    decision.flash = settings.flash && !focused;
    return decision;
}

Decision decide_event(const Settings& settings, std::optional<bool> focused, std::time_t now) {
    Decision decision;
    if (dnd(settings, now)) {
        return decision;
    }
    const bool front = focused.value_or(false);
    decision.desktop = settings.desktop && settings.events && (!front || settings.when_focused);
    decision.flash = settings.flash && !front;
    return decision;
}

Shown text(const Settings& settings, std::string_view who, std::string_view where, bool direct, std::string_view what) {
    switch (settings.preview) {
    case Preview::Full:
        return {std::string(where), std::string(who), std::string(what)};
    case Preview::Name:
        return {std::string(where), std::string(who), direct ? "sent you a private message" : "sent a message"};
    case Preview::Hidden:
        break;
    }
    return {"zchat", "New message", "", false};
}

std::string_view name(Level level) {
    switch (level) {
    case Level::Default:
        return "default";
    case Level::All:
        return "all";
    case Level::Mentions:
        return "mentions";
    case Level::Nothing:
        return "nothing";
    }
    return "default";
}

std::string_view name(Preview preview) {
    switch (preview) {
    case Preview::Full:
        return "full";
    case Preview::Name:
        return "name";
    case Preview::Hidden:
        return "hidden";
    }
    return "full";
}

std::optional<Level> parse_level(std::string_view text) {
    if (text == "default") {
        return Level::Default;
    }
    if (text == "all") {
        return Level::All;
    }
    if (text == "mentions" || text == "tags") {
        return Level::Mentions;
    }
    if (text == "nothing" || text == "none" || text == "mute" || text == "muted") {
        return Level::Nothing;
    }
    return std::nullopt;
}

std::optional<Preview> parse_preview(std::string_view text) {
    if (text == "full" || text == "message") {
        return Preview::Full;
    }
    if (text == "name") {
        return Preview::Name;
    }
    if (text == "hidden" || text == "none") {
        return Preview::Hidden;
    }
    return std::nullopt;
}

std::optional<std::time_t> parse_duration(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::time_t unit = 60;
    switch (text.back()) {
    case 'm':
        text.remove_suffix(1);
        break;
    case 'h':
        unit = 60 * 60;
        text.remove_suffix(1);
        break;
    case 'd':
        unit = 24 * 60 * 60;
        text.remove_suffix(1);
        break;
    default:
        break;
    }
    const auto count = parse_number<long long>(text);
    // Up to a year.
    if (!count || *count <= 0 || *count * unit > 366LL * 24 * 60 * 60) {
        return std::nullopt;
    }
    return static_cast<std::time_t>(*count * unit);
}

std::vector<std::pair<std::string, std::string>> to_config(const Settings& s) {
    std::vector<std::pair<std::string, std::string>> out = {
        {"notify_level", std::string(name(s.level))},
        // (The keys and values of the first notifications, kept.)
        {"notifications", std::string(on_off(s.desktop))},
        {"notify_messages", std::string(on_off(s.messages))},
        {"notify_events", std::string(on_off(s.events))},
        {"notify_focused", std::string(on_off(s.when_focused))},
        {"notify_preview", std::string(name(s.preview))},
        {"notify_sound", std::string(yes_no(s.sound))},
        {"notify_sound_name", s.sound_name},
        {"notify_volume", std::to_string(s.volume)},
        {"notify_flash", std::string(yes_no(s.flash))},
        {"notify_badge", std::string(yes_no(s.badge))},
        {"notify_suppress_everyone", std::string(yes_no(s.suppress_everyone))},
        {"notify_dnd_until", s.dnd_until == dnd_forever ? std::string("forever") : std::to_string(s.dnd_until)},
        {"notify_dnd_trills", std::string(yes_no(s.dnd_trills))},
    };
    for (const auto& [channel, level] : s.channels) {
        if (level != Level::Default) {
            out.emplace_back(std::format("{}{}", channel_prefix, channel), std::string(name(level)));
        }
    }
    return out;
}

Settings from_config(const std::function<std::optional<std::string>(std::string_view key)>& get) {
    Settings s;
    const auto boolean = [&](std::string_view key, bool& into) {
        if (const auto v = get(key); v) {
            into = parse_bool(*v).value_or(into);
        }
    };
    if (const auto v = get("notify_level"); v) {
        if (const auto level = parse_level(*v); level && level != Level::Default) {
            s.level = *level;
        }
    }
    boolean("notifications", s.desktop);
    boolean("notify_messages", s.messages);
    boolean("notify_events", s.events);
    boolean("notify_focused", s.when_focused);
    if (const auto v = get("notify_preview"); v) {
        s.preview = parse_preview(*v).value_or(s.preview);
    }
    boolean("notify_sound", s.sound);
    if (const auto v = get("notify_sound_name"); v && known_sound(*v)) {
        s.sound_name = *v;
    }
    if (const auto v = get("notify_volume"); v) {
        s.volume = std::clamp(parse_number<int>(*v).value_or(s.volume), 0, 100);
    }
    boolean("notify_flash", s.flash);
    boolean("notify_badge", s.badge);
    boolean("notify_suppress_everyone", s.suppress_everyone);
    if (const auto v = get("notify_dnd_until"); v) {
        s.dnd_until = *v == "forever" ? dnd_forever : parse_number<long long>(*v).value_or(0);
    }
    boolean("notify_dnd_trills", s.dnd_trills);
    return s;
}

std::string to_json(const Settings& s) {
    std::string channels = "{";
    for (const auto& [channel, level] : s.channels) {
        if (level != Level::Default) {
            channels += std::format("{}{}:{}", channels.size() > 1 ? "," : "", json::quote(channel),
                                    json::quote(name(level)));
        }
    }
    channels += "}";
    // dnd_until: -1 for forever, else seconds since 1970 (0 when off).
    return std::format("{{\"level\":{},\"desktop\":{},\"messages\":{},\"events\":{},\"when_focused\":{},"
                       "\"preview\":{},\"sound\":{},\"sound_name\":{},\"volume\":{},\"flash\":{},\"badge\":{},"
                       "\"suppress_everyone\":{},\"dnd_until\":{},\"dnd_trills\":{},\"channels\":{}}}",
                       json::quote(name(s.level)), s.desktop, s.messages, s.events, s.when_focused,
                       json::quote(name(s.preview)), s.sound,
                       json::quote(s.sound_name), s.volume, s.flash, s.badge, s.suppress_everyone,
                       s.dnd_until == dnd_forever ? -1LL : static_cast<long long>(s.dnd_until), s.dnd_trills,
                       channels);
}

Settings from_json(std::string_view text, Settings s) {
    const auto parsed = json::parse(text);
    if (!parsed || !parsed->is_object()) {
        return s;
    }
    const json::Value& v = *parsed;
    const auto boolean = [&](std::string_view key, bool& into) {
        if (const auto* b = v.find(key); b && b->is_bool()) {
            into = b->boolean;
        }
    };
    if (const auto level = parse_level(v["level"].str()); level && level != Level::Default) {
        s.level = *level;
    }
    boolean("desktop", s.desktop);
    boolean("messages", s.messages);
    boolean("events", s.events);
    boolean("when_focused", s.when_focused);
    s.preview = parse_preview(v["preview"].str()).value_or(s.preview);
    boolean("sound", s.sound);
    if (known_sound(v["sound_name"].str())) {
        s.sound_name = std::string(v["sound_name"].str());
    }
    if (v["volume"].is_number()) {
        s.volume = std::clamp(v["volume"].integer(s.volume), 0, 100);
    }
    boolean("flash", s.flash);
    boolean("badge", s.badge);
    boolean("suppress_everyone", s.suppress_everyone);
    if (const auto* until = v.find("dnd_until"); until && until->is_number()) {
        s.dnd_until = until->number < 0 ? dnd_forever : static_cast<std::time_t>(until->number);
    }
    boolean("dnd_trills", s.dnd_trills);
    if (const auto* channels = v.find("channels"); channels && channels->is_object()) {
        s.channels.clear();
        for (std::size_t i = 0; i < channels->keys.size(); ++i) {
            if (const auto level = parse_level(channels->items[i].str()); level && level != Level::Default) {
                s.channels[channels->keys[i]] = *level;
            }
        }
    }
    return s;
}

Settings current() {
    std::scoped_lock lock(mutex);
    if (!loaded) {
        // The file is read once.
        const auto values = config::all();
        loaded = from_config([&values](std::string_view key) -> std::optional<std::string> {
            const auto it = std::ranges::find(values, key, &std::pair<std::string, std::string>::first);
            return it == values.end() ? std::nullopt : std::optional<std::string>(it->second);
        });
        for (const auto& [key, value] : values) {
            if (key.starts_with(channel_prefix)) {
                if (const auto level = parse_level(value); level && level != Level::Default) {
                    loaded->channels[key.substr(channel_prefix.size())] = *level;
                }
            }
        }
    }
    return *loaded;
}

void set(Settings settings) {
    std::erase_if(settings.channels, [](const auto& channel) {
        return channel.second == Level::Default;
    });
    std::scoped_lock lock(mutex);
    // The channels no longer set lose their keys.
    std::vector<std::string> removed;
    for (const auto& [key, value] : config::all()) {
        if (key.starts_with(channel_prefix) && !settings.channels.contains(key.substr(channel_prefix.size()))) {
            removed.push_back(key);
        }
    }
    config::update(to_config(settings), removed);
    loaded = settings;
}

} // namespace zchat::notify
