// Tests of the notification settings and of what a message calls for (notify.hpp).

#include "check.hpp"

#include "notify.hpp"

#include <map>
#include <string>

using namespace zchat::notify;

namespace {

constexpr std::time_t now = 1'000'000;

Message message(bool tags_us = false, bool direct = false) {
    Message m;
    m.channel = direct ? "dm-1" : "dev";
    m.direct = direct;
    m.tags_us = tags_us;
    m.focused = false;
    return m;
}

} // namespace

TEST(notify_levels) {
    Settings s;
    // Mentions by default: a plain message only counts.
    auto d = decide(s, message(), now);
    CHECK(d.counts);
    CHECK(!d.sound);
    CHECK(!d.desktop);
    d = decide(s, message(true), now);
    CHECK(d.sound && d.desktop && d.flash);
    // Private messages always notify.
    CHECK(decide(s, message(false, true), now).desktop);
    s.level = Level::All;
    CHECK(decide(s, message(), now).desktop);
    // A channel of its own level.
    s.channels["dev"] = Level::Nothing;
    d = decide(s, message(true), now);
    CHECK(!d.counts && !d.sound && !d.desktop);
    s.channels["dm-1"] = Level::Mentions;
    CHECK(decide(s, message(false, true), now).desktop);
    s.channels["dm-1"] = Level::Nothing;
    CHECK(!decide(s, message(false, true), now).sound);
}

TEST(notify_own_and_focus) {
    Settings s;
    auto m = message(true);
    m.own = true;
    CHECK(!decide(s, m, now).counts);
    // Looking at it: nothing.
    m = message(true);
    m.focused = true;
    m.current = true;
    auto d = decide(s, m, now);
    CHECK(!d.sound && !d.desktop && !d.flash);
    // In front, but elsewhere: the sound only, unless asked.
    m.current = false;
    d = decide(s, m, now);
    CHECK(d.sound && !d.desktop && !d.flash);
    s.when_focused = true;
    CHECK(decide(s, m, now).desktop);
    // Not known: as if not in front.
    m.focused = std::nullopt;
    m.current = true;
    CHECK(decide(s, m, now).flash);
}

TEST(notify_dnd) {
    Settings s;
    s.dnd_until = now + 60;
    CHECK(dnd(s, now));
    auto d = decide(s, message(true), now);
    CHECK(d.counts && !d.sound && !d.desktop && !d.flash);
    CHECK(!dnd(s, now + 60));
    CHECK(decide(s, message(true), now + 61).sound);
    s.dnd_until = dnd_forever;
    CHECK(dnd(s, now + 1'000'000'000));
    s.dnd_until = 0;
    CHECK(!dnd(s, now));
}

TEST(notify_everyone) {
    Settings s;
    auto m = message(true);
    m.only_everyone = true;
    CHECK(decide(s, m, now).sound);
    s.suppress_everyone = true;
    CHECK(!decide(s, m, now).sound);
    m.only_everyone = false;
    CHECK(decide(s, m, now).sound);
}

TEST(notify_preview) {
    Settings s;
    auto shown = text(s, "Rex", "#dev", false, "hello");
    CHECK_EQ(shown.where, std::string("#dev"));
    CHECK_EQ(shown.title, std::string("Rex"));
    CHECK_EQ(shown.text, std::string("hello"));
    CHECK(shown.who);
    s.preview = Preview::Name;
    shown = text(s, "Rex", "Private message", true, "hello");
    CHECK_EQ(shown.title, std::string("Rex"));
    CHECK(shown.text != "hello");
    s.preview = Preview::Hidden;
    shown = text(s, "Rex", "#dev", false, "hello");
    CHECK(!shown.who);
    CHECK(shown.where.find("dev") == std::string::npos);
    CHECK(shown.title.find("Rex") == std::string::npos && shown.text.find("hello") == std::string::npos);
}

TEST(notify_switches) {
    Settings s;
    s.messages = false;
    auto d = decide(s, message(true), now);
    CHECK(!d.desktop && d.sound);
    CHECK(decide_event(s, false, now).desktop);
    s.events = false;
    CHECK(!decide_event(s, false, now).desktop);
    s.events = true;
    // In front: only with when_focused; Do Not Disturb: nothing.
    CHECK(!decide_event(s, true, now).desktop);
    s.when_focused = true;
    CHECK(decide_event(s, true, now).desktop);
    s.dnd_until = dnd_forever;
    d = decide_event(s, false, now);
    CHECK(!d.desktop && !d.flash);
    // The keys of the first notifications are read.
    const Settings old = from_config([](std::string_view key) -> std::optional<std::string> {
        if (key == "notifications" || key == "notify_events") {
            return "off";
        }
        return key == "notify_focused" ? std::optional<std::string>("on") : std::nullopt;
    });
    CHECK(!old.desktop && !old.events && old.messages && old.when_focused);
}

TEST(notify_durations) {
    CHECK_EQ(parse_duration("30m").value_or(0), 30 * 60);
    CHECK_EQ(parse_duration("15").value_or(0), 15 * 60);
    CHECK_EQ(parse_duration("8h").value_or(0), 8 * 60 * 60);
    CHECK_EQ(parse_duration("2d").value_or(0), 2 * 24 * 60 * 60);
    CHECK(!parse_duration(""));
    CHECK(!parse_duration("0m"));
    CHECK(!parse_duration("soon"));
    CHECK(!parse_duration("1000d"));
}

TEST(notify_config_round_trip) {
    Settings s;
    s.level = Level::All;
    s.desktop = false;
    s.preview = Preview::Name;
    s.sound_name = "chime";
    s.volume = 35;
    s.suppress_everyone = true;
    s.dnd_until = dnd_forever;
    s.taskbar = false;
    std::map<std::string, std::string, std::less<>> file;
    for (const auto& [k, v] : to_config(s)) {
        file[k] = v;
    }
    const Settings back = from_config([&](std::string_view key) -> std::optional<std::string> {
        const auto it = file.find(key);
        return it == file.end() ? std::nullopt : std::optional<std::string>(it->second);
    });
    CHECK(back.level == Level::All);
    CHECK(!back.desktop);
    CHECK(back.preview == Preview::Name);
    CHECK_EQ(back.sound_name, std::string("chime"));
    CHECK_EQ(back.volume, 35);
    CHECK(back.suppress_everyone);
    CHECK(back.dnd_until == dnd_forever);
    CHECK(!back.taskbar);
    // Nonsense is ignored.
    file["notify_sound_name"] = "../../evil";
    file["notify_volume"] = "999";
    const Settings odd = from_config([&](std::string_view key) -> std::optional<std::string> {
        const auto it = file.find(key);
        return it == file.end() ? std::nullopt : std::optional<std::string>(it->second);
    });
    CHECK_EQ(odd.sound_name, std::string("ding"));
    CHECK_EQ(odd.volume, 100);
}

TEST(notify_json_round_trip) {
    Settings s;
    s.channels["dev"] = Level::Nothing;
    s.channels["dm-1"] = Level::All;
    s.dnd_until = dnd_forever;
    s.when_focused = true;
    const Settings back = from_json(to_json(s), Settings {});
    CHECK(back.channels.size() == 2);
    CHECK(back.channels.at("dev") == Level::Nothing);
    CHECK(back.dnd_until == dnd_forever);
    CHECK(back.when_focused);
    // What is not said stays.
    Settings base;
    base.volume = 12;
    const Settings partial = from_json(R"({"sound":false})", base);
    CHECK(!partial.sound);
    CHECK_EQ(partial.volume, 12);
    CHECK(partial.channels.empty());
    // Not JSON: unchanged.
    CHECK_EQ(from_json("nope", base).volume, 12);
}
