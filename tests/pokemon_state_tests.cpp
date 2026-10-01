// Tests of pokemon::Battle: real battles from the simulator replayed as each viewer saw them (tests/pokemon/*.log, as
// zchat's bridge prints them), and small hand-written messages for the tricky cases.

#include "check.hpp"

#include "json.hpp"
#include "pokemon_state.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using zchat::pokemon::Battle;
using zchat::pokemon::LogLine;

namespace {

// A line of a log: the stream it was for (spectator, p1, p2, ready, exit, error) and the message.
struct Message {
    std::string stream;
    std::string text;
};

// The bridge doubles backslashes and writes line breaks as \n.
std::string unescape(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += s[i] == 'n' ? '\n' : s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<Message> load(std::string_view name) {
    std::vector<Message> messages;
    std::ifstream in(std::string(ZCHAT_TEST_DATA) + "/" + std::string(name), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto space = line.find(' ');
        messages.push_back({line.substr(0, space), space == std::string::npos ? "" : unescape(line.substr(space + 1))});
    }
    return messages;
}

const char* const logs[] = {"battle-weather.log", "battle-hazards-drag.log", "battle-error.log"};

// The stream a viewer is fed.
std::string stream_of(std::string_view side) {
    return side.empty() ? "spectator" : std::string(side);
}

// Feeds a viewer its messages, answering each request as a player does (the choice is not checked here).
Battle replay(std::string_view name, std::string_view side) {
    Battle battle {std::string(side)};
    for (const Message& m : load(name)) {
        if (m.stream == stream_of(side)) {
            battle.feed(m.text);
        }
    }
    return battle;
}

// The last |turn| of a stream.
int last_turn(std::string_view name) {
    int turn = 0;
    for (const Message& m : load(name)) {
        if (m.stream != "spectator") {
            continue;
        }
        for (std::size_t at = m.text.find("|turn|"); at != std::string::npos; at = m.text.find("|turn|", at + 1)) {
            turn = std::stoi(m.text.substr(at + 6));
        }
    }
    return turn;
}

bool has_line(const Battle& b, std::string_view text, std::optional<LogLine::Kind> kind = std::nullopt) {
    return std::ranges::any_of(b.log(), [&](const LogLine& l) {
        return l.text == text && (!kind || l.kind == *kind);
    });
}

zchat::json::Value parsed(const Battle& b, std::string_view extra = {}) {
    const auto value = zchat::json::parse(b.json(extra));
    REQUIRE(value.has_value());
    REQUIRE(value->is_object());
    return *value;
}

// A Pokémon of a side in json(), by name.
const zchat::json::Value* mon(const zchat::json::Value& state, int side, std::string_view name) {
    for (const auto& m : state["sides"][static_cast<std::size_t>(side)]["team"].items) {
        if (m["name"].str() == name) {
            return &m;
        }
    }
    return nullptr;
}

bool contains(const zchat::json::Value& array, std::string_view text) {
    return std::ranges::any_of(array.items, [&](const zchat::json::Value& v) {
        return v.str() == text;
    });
}

// Feeds a viewer the messages of its stream after the ones already fed, up to the first one containing what,
// included, then calls then.
struct Replay {
    Battle battle;
    std::vector<Message> messages;
    std::size_t next = 0;

    Replay(std::string_view name, std::string_view side) :
        battle(std::string(side)),
        messages(load(name)) {
    }

    template <typename F>
    void until(std::string_view what, F&& then) {
        while (next < messages.size()) {
            const Message& m = messages[next++];
            if (m.stream != stream_of(battle.side())) {
                continue;
            }
            battle.feed(m.text);
            if (m.text.find(what) != std::string::npos) {
                then();
                return;
            }
        }
        CHECK(!"the message was not found");
    }
};

} // namespace

TEST(pokemon_logs_replay_for_every_viewer) {
    for (const char* name : logs) {
        const bool won = load(name).back().stream == "exit";
        for (const char* side : {"p1", "p2", ""}) {
            const Battle b = replay(name, side);
            CHECK_EQ(b.turn(), last_turn(name));
            CHECK_EQ(b.over(), won);
            CHECK_EQ(b.winner(), std::string(won ? "Ash" : ""));
            CHECK_EQ(b.player_name("p1"), std::string("Ash"));
            CHECK_EQ(b.player_name("p2"), std::string("Gary"));
            CHECK(!b.log().empty());
            const auto state = parsed(b, R"("extra": 1)");
            CHECK_EQ(state["extra"].integer(), 1);
            for (const char* key : {"you", "turn", "over", "winner", "sides", "field", "request", "error", "log"}) {
                CHECK(state.find(key) != nullptr);
            }
            CHECK_EQ(state["you"].str(), std::string_view(side));
            CHECK_EQ(state["turn"].integer(), b.turn());
            CHECK_EQ(state["sides"].size(), 2u);
            CHECK_EQ(state["sides"][0]["id"].str(), std::string_view("p1"));
            CHECK_EQ(state["sides"][1]["name"].str(), std::string_view("Gary"));
            CHECK(state["field"]["other"].is_array());
            for (int s = 0; s < 2; ++s) {
                const auto& sd = state["sides"][static_cast<std::size_t>(s)];
                CHECK(sd["team"].size() >= 1 && sd["team"].size() <= 6);
                CHECK(sd["active"].integer() >= 0 && sd["active"].integer() < static_cast<int>(sd["team"].size()));
                for (const auto& m : sd["team"].items) {
                    for (const char* key :
                         {"name", "species", "level", "gender", "shiny", "hp", "maxhp", "exact", "status", "types",
                          "tera", "teraType", "boosts", "volatiles", "item", "ability", "moves", "stats"}) {
                        CHECK(m.find(key) != nullptr);
                    }
                    CHECK(m["hp"].integer() >= 0 && m["hp"].integer() <= m["maxhp"].integer());
                    CHECK(m["types"].size() >= 1);
                }
            }
            if (won) {
                CHECK(state["request"].is_null());
                CHECK_EQ(b.rqid(), 0);
                CHECK(has_line(b, "Ash won the battle!", LogLine::Kind::Result));
            }
            for (const auto& l : state["log"].items) {
                CHECK(!l["t"].str().empty());
                CHECK(l["t"].str().find("**") == std::string_view::npos);
                CHECK(l["t"].str().find('[') == std::string_view::npos || l["t"].str()[0] == '[');
            }
        }
    }
}

TEST(pokemon_exact_hp_only_for_our_side) {
    const Battle spectator = replay("battle-weather.log", "");
    const auto s = parsed(spectator);
    for (int side = 0; side < 2; ++side) {
        for (const auto& m : s["sides"][static_cast<std::size_t>(side)]["team"].items) {
            CHECK(!m["exact"].boolean);
            CHECK_EQ(m["maxhp"].integer(), 100);
            CHECK_EQ(m["stats"].size(), 0u);
            CHECK_EQ(m["teraType"].str(), std::string_view());
        }
    }
    const Battle p1 = replay("battle-weather.log", "p1");
    const auto state = parsed(p1);
    // Ours: the whole team, exact.
    CHECK_EQ(state["sides"][0]["team"].size(), 6u);
    const auto* moth = mon(state, 0, "Iron Moth");
    REQUIRE(moth);
    CHECK((*moth)["exact"].boolean);
    CHECK_EQ((*moth)["maxhp"].integer(), 253);
    CHECK_EQ((*moth)["status"].str(), std::string_view("fnt"));
    CHECK_EQ((*moth)["item"].str(), std::string_view("Heavy-Duty Boots"));
    CHECK_EQ((*moth)["ability"].str(), std::string_view("Quark Drive"));
    CHECK_EQ((*moth)["teraType"].str(), std::string_view("Grass"));
    CHECK_EQ((*moth)["stats"]["spa"].integer(), 263);
    CHECK(contains((*moth)["moves"], "Toxic Spikes"));
    // Theirs: out of 100, as seen.
    const auto* latias = mon(state, 1, "Latias");
    REQUIRE(latias);
    CHECK(!(*latias)["exact"].boolean);
    CHECK_EQ((*latias)["maxhp"].integer(), 100);
    CHECK_EQ((*latias)["status"].str(), std::string_view("fnt"));
    CHECK_EQ((*latias)["item"].str(), std::string_view());
    CHECK_EQ((*latias)["ability"].str(), std::string_view("Levitate"));
    CHECK(contains((*latias)["moves"], "Draco Meteor"));
    CHECK(contains((*latias)["moves"], "Recover"));
    CHECK(contains((*latias)["types"], "Dragon"));
    CHECK_EQ(state["sides"][1]["team"].size(), 6u);
    // Its Choice Band was knocked off: it was seen, then lost.
    const auto* emboar = mon(state, 1, "Emboar");
    REQUIRE(emboar);
    CHECK_EQ((*emboar)["item"].str(), std::string_view());
}

TEST(pokemon_terastallize) {
    for (const char* side : {"p1", "p2", ""}) {
        const auto state = parsed(replay("battle-weather.log", side));
        CHECK_EQ((*mon(state, 0, "Iron Moth"))["tera"].str(), std::string_view("Grass"));
        CHECK_EQ((*mon(state, 1, "Leavanny"))["tera"].str(), std::string_view("Ghost"));
        CHECK_EQ((*mon(state, 0, "Sylveon"))["tera"].str(), std::string_view());
    }
    const Battle p2 = replay("battle-weather.log", "p2");
    CHECK(has_line(p2, "Leavanny has Terastallized into the Ghost-type!"));
    CHECK(has_line(p2, "The opposing Iron Moth has Terastallized into the Grass-type!"));
    // A Terastallized Pokémon switching back in says so in its details.
    const auto hazards = parsed(replay("battle-hazards-drag.log", ""));
    CHECK_EQ((*mon(hazards, 1, "Seviper"))["tera"].str(), std::string_view("Ground"));
}

TEST(pokemon_weather) {
    Replay replay_b("battle-weather.log", "p1");
    Battle& b = replay_b.battle;
    replay_b.until("|-weather|Sandstorm|[from] ability: Sand Stream", [&] {
        const auto state = parsed(b);
        CHECK_EQ(state["field"]["weather"].str(), std::string_view("Sandstorm"));
        CHECK(has_line(b, "[Tyranitar's Sand Stream]", LogLine::Kind::Minor));
        CHECK(has_line(b, "A sandstorm kicked up!"));
    });
    replay_b.until("|-weather|Sandstorm|[upkeep]", [&] {
        CHECK_EQ(parsed(b)["field"]["weather"].str(), std::string_view("Sandstorm"));
        CHECK(has_line(b, "(The sandstorm is raging.)", LogLine::Kind::Minor));
    });
    replay_b.until("|-weather|none", [&] {
        CHECK_EQ(parsed(b)["field"]["weather"].str(), std::string_view());
        CHECK(has_line(b, "The sandstorm subsided."));
        CHECK(has_line(b, "The opposing Latias is buffeted by the sandstorm!", LogLine::Kind::Minor));
    });
}

TEST(pokemon_side_conditions) {
    Replay replay_b("battle-hazards-drag.log", "");
    Battle& b = replay_b.battle;
    replay_b.until("|turn|8", [&] {
        const auto state = parsed(b);
        CHECK(contains(state["sides"][0]["conditions"], "Spikes"));
        CHECK(has_line(b, "Spikes were scattered on the ground all around your team!"));
    });
    replay_b.until("|turn|11", [&] {
        CHECK(contains(parsed(b)["sides"][0]["conditions"], "Spikes (2)"));
    });
    replay_b.until("|drag|p1a: Ting-Lu", [&] {
        CHECK(has_line(b, "Ting-Lu was dragged out!"));
        CHECK(has_line(b, "Ting-Lu was hurt by the spikes!", LogLine::Kind::Minor));
        CHECK(has_line(b, "[Ting-Lu's Vessel of Ruin]"));
        CHECK(has_line(b, "Ting-Lu's Vessel of Ruin weakened the Sp. Atk of all surrounding Pokémon!"));
    });

    Replay replay_p2("battle-weather.log", "p2");
    Battle& p2 = replay_p2.battle;
    replay_p2.until("|-sidestart|p2: Gary|move: Stealth Rock", [&] {
        const auto state = parsed(p2);
        CHECK(contains(state["sides"][1]["conditions"], "Stealth Rock"));
        CHECK(contains(state["sides"][1]["conditions"], "Toxic Spikes"));
        CHECK(contains(state["sides"][0]["conditions"], "Sticky Web"));
        CHECK(has_line(p2, "Pointed stones float in the air around your team!"));
        CHECK(has_line(p2, "Poison spikes were scattered on the ground all around your team!"));
        CHECK(has_line(p2, "A sticky web has been laid out on the ground around the opposing team!"));
        CHECK(has_line(p2, "The opposing Walking Wake was caught in a sticky web!"));
    });
    // Defog clears them all.
    replay_p2.until("|-sideend|p1: Ash|Sticky Web|[from] move: Defog", [&] {
        const auto state = parsed(p2);
        CHECK_EQ(state["sides"][0]["conditions"].size(), 0u);
        CHECK_EQ(state["sides"][1]["conditions"].size(), 0u);
        CHECK(has_line(p2, "The pointed stones disappeared from around your team!"));
        CHECK(has_line(p2, "The sticky web has disappeared from the ground around the opposing team!"));
    });
}

TEST(pokemon_log_words) {
    const Battle p1 = replay("battle-weather.log", "p1");
    CHECK(has_line(p1, "Battle started between Ash and Gary!"));
    CHECK(has_line(p1, "Turn 1", LogLine::Kind::Turn));
    CHECK(has_line(p1, "Go! Iron Moth!"));
    CHECK(has_line(p1, "Gary sent out Leavanny!"));
    CHECK(has_line(p1, "Iron Moth used Fire Blast!", LogLine::Kind::Move));
    CHECK(has_line(p1, "The opposing Leavanny used Sticky Web!", LogLine::Kind::Move));
    CHECK(has_line(p1, "(The opposing Leavanny lost 49% of its health!)", LogLine::Kind::Minor));
    // Our HP is exact: so is what we lose.
    CHECK(has_line(p1, "(Iron Moth lost 70.4% of its health!)", LogLine::Kind::Minor));
    CHECK(has_line(p1, "The opposing Leavanny fainted!"));
    CHECK(has_line(p1, "Iron Moth fainted!"));
    CHECK(has_line(p1, "It's super effective!"));
    CHECK(has_line(p1, "It's not very effective..."));
    CHECK(has_line(p1, "A critical hit!"));
    CHECK(has_line(p1, "Iron Moth avoided the attack!") == false);
    CHECK(has_line(p1, "The opposing Kingdra avoided the attack!"));
    CHECK(has_line(p1, "It doesn't affect Sylveon..."));
    CHECK(has_line(p1, "[The opposing Latias's Levitate]"));
    CHECK(has_line(p1, "It doesn't affect the opposing Latias..."));
    CHECK(has_line(p1, "The opposing Kingdra became confused due to fatigue!"));
    CHECK(has_line(p1, "(The opposing Kingdra ate its Lum Berry!)", LogLine::Kind::Minor));
    CHECK(has_line(p1, "The opposing Kingdra snapped out of its confusion!"));
    CHECK(has_line(p1, "The opposing Kingdra is confused!"));
    CHECK(has_line(p1, "It hurt itself in its confusion!"));
    CHECK(has_line(p1, "The opposing Kingdra was damaged by the recoil!"));
    CHECK(has_line(p1, "Sylveon flinched and couldn't move!"));
    CHECK(has_line(p1, "Sylveon's wish came true!"));
    CHECK(has_line(p1, "Sylveon restored a little HP using its Leftovers!", LogLine::Kind::Minor));
    CHECK(has_line(p1, "Sylveon's Sp. Atk rose!"));
    CHECK(has_line(p1, "Sylveon protected itself!"));
    CHECK(has_line(p1, "The opposing Perrserker knocked off Sylveon's Leftovers!"));
    CHECK(has_line(p1, "The opposing Perrserker lost some of its HP!"));
    CHECK(has_line(p1, "The opposing Perrserker's Defense fell!"));
    CHECK(has_line(p1, "The opposing Lucario's Sp. Atk rose sharply!"));
    CHECK(has_line(p1, "The opposing Latias's Sp. Atk fell harshly!"));
    CHECK(has_line(p1, "The opposing Kingdra was poisoned!"));
    CHECK(has_line(p1, "The opposing Kingdra was hurt by poison!"));
    CHECK(has_line(p1, "Pointed stones dug into the opposing Latias!"));
    CHECK(has_line(p1, "The opposing Latias's HP is full!"));
    CHECK(has_line(p1, "But it failed!"));
    CHECK(has_line(p1, "The opposing Latias had its HP restored."));
    CHECK(has_line(p1, "Ash won the battle!", LogLine::Kind::Result));
    // [silent] lines say nothing.
    CHECK(!has_line(p1, "The effects of Iron Moth's Quark Drive wore off!"));

    const Battle spectator = replay("battle-weather.log", "");
    // A spectator sees from p1's side, but nobody's Pokémon are their own.
    CHECK(has_line(spectator, "Ash sent out Iron Moth!"));
    CHECK(has_line(spectator, "Gary sent out Leavanny!"));
    CHECK(has_line(spectator, "The opposing Leavanny fainted!"));
    CHECK(has_line(spectator, "(Iron Moth lost 70% of its health!)"));
    CHECK(!has_line(spectator, "Go! Iron Moth!"));
    // A voluntary switch withdraws the one that was in.
    CHECK(has_line(spectator, "Gary withdrew Kingdra!"));

    const Battle p2 = replay("battle-weather.log", "p2");
    CHECK(has_line(p2, "The opposing Iron Moth used Fire Blast!"));
    CHECK(has_line(p2, "Go! Leavanny!"));
    CHECK(has_line(p2, "Kingdra, come back!"));
    CHECK(has_line(p2, "Ash sent out Iron Moth!"));

    const Battle errors = replay("battle-error.log", "p1");
    CHECK(has_line(errors, "The opposing Alomomola protected itself!"));
    // The wisher is named as it is, near or not.
    CHECK(has_line(errors, "Alomomola's wish came true!"));
    CHECK(has_line(errors, "The Pokémon was hit 2 times!"));
    CHECK(has_line(errors, "The Pokémon was hit 1 time!"));
    CHECK(has_line(errors, "The opposing Alomomola was burned!"));
    CHECK(has_line(errors, "The opposing Alomomola was hurt by its burn!"));
    CHECK(has_line(errors, "Hypno was seeded!"));
    CHECK(has_line(errors, "Hypno's health is sapped by Leech Seed!"));
    CHECK(has_line(errors, "Pawmot fell asleep!"));
    CHECK(has_line(errors, "Pawmot is fast asleep."));
    CHECK(has_line(errors, "Pawmot woke up!"));
    CHECK(has_line(errors, "[The opposing Salamence's Intimidate]"));
    CHECK(has_line(errors, "Pawmot's Attack fell!"));
    CHECK(has_line(errors, "Pawmot used up all of its electricity!"));
    CHECK(has_line(errors, "Pawmot restored PP to its move Revival Blessing using its Leppa Berry!"));
    CHECK(has_line(errors, "The opposing Lilligant's Speed rose!"));
    CHECK(has_line(errors, "The opposing Meganium knocked off Dragapult's Life Orb!"));

    const Battle hazards = replay("battle-hazards-drag.log", "p1");
    CHECK(has_line(hazards, "The opposing Seviper was frozen solid!"));
    CHECK(has_line(hazards, "The opposing Seviper is frozen solid!"));
    CHECK(has_line(hazards, "The opposing Seviper thawed out!"));
    CHECK(has_line(hazards, "Electivire kept going and crashed!"));
    CHECK(has_line(hazards, "Lurantis is paralyzed! It may be unable to move!"));
    CHECK(has_line(hazards, "Lurantis is paralyzed! It can't move!"));
    CHECK(has_line(hazards, "Lurantis is already paralyzed!"));
    CHECK(has_line(hazards, "The opposing Forretress had its energy drained!"));
    CHECK(has_line(hazards, "Lurantis's Sp. Atk rose sharply!"));
}

TEST(pokemon_types_boosts_and_volatiles) {
    Replay replay_b("battle-error.log", "p1");
    Battle& b = replay_b.battle;
    replay_b.until("|-start|p1a: Pawmot|typechange|???/Fighting", [&] {
        const auto state = parsed(b);
        const auto* pawmot = mon(state, 0, "Pawmot");
        REQUIRE(pawmot);
        CHECK(contains((*pawmot)["types"], "???"));
        CHECK(contains((*pawmot)["types"], "Fighting"));
        CHECK(!contains((*pawmot)["types"], "Electric"));
        const auto* salamence = mon(state, 1, "Salamence");
        REQUIRE(salamence);
        CHECK(contains((*salamence)["types"], "Flying"));
        CHECK_EQ((*salamence)["ability"].str(), std::string_view("Intimidate"));
    });
    Replay replay_hypno("battle-error.log", "p1");
    Battle& hypno = replay_hypno.battle;
    replay_hypno.until("|-start|p1a: Hypno|move: Leech Seed", [&] {
        const auto state = parsed(hypno);
        CHECK(contains((*mon(state, 0, "Hypno"))["volatiles"], "Leech Seed"));
    });
    // Switching out clears them.
    replay_hypno.until("|switch|p1a: Ariados", [&] {
        const auto state = parsed(hypno);
        CHECK_EQ((*mon(state, 0, "Hypno"))["volatiles"].size(), 0u);
        CHECK_EQ(
            state["sides"][0]["team"][static_cast<std::size_t>(state["sides"][0]["active"].integer())]["name"].str(),
            std::string_view("Ariados"));
    });
    Replay replay_lilligant("battle-error.log", "");
    Battle& lilligant = replay_lilligant.battle;
    replay_lilligant.until("|switch|p1a: Ariados", [&] {
        const auto state = parsed(lilligant);
        const auto* l = mon(state, 1, "Lilligant");
        REQUIRE(l);
        CHECK_EQ((*l)["boosts"]["atk"].integer(), 2);
        CHECK_EQ((*l)["boosts"]["spe"].integer(), 2);
        CHECK_EQ((*l)["species"].str(), std::string_view("Lilligant-Hisui"));
        CHECK_EQ((*l)["gender"].str(), std::string_view("F"));
        CHECK_EQ((*l)["level"].integer(), 79);
    });
}

TEST(pokemon_requests) {
    Replay replay_b("battle-weather.log", "p1");
    Battle& b = replay_b.battle;
    CHECK_EQ(b.rqid(), 0);
    std::string error;
    CHECK(!b.parse_choice("1", error));
    CHECK(!error.empty());
    replay_b.until("|request|", [&] {
        const int rqid = b.rqid();
        CHECK(rqid > 0);
        const auto state = parsed(b);
        const auto& q = state["request"];
        CHECK_EQ(q["rqid"].integer(), rqid);
        CHECK_EQ(q["kind"].str(), std::string_view("move"));
        CHECK_EQ(q["moves"].size(), 4u);
        CHECK_EQ(q["moves"][0]["slot"].integer(), 1);
        CHECK_EQ(q["moves"][0]["name"].str(), std::string_view("Fire Blast"));
        CHECK_EQ(q["moves"][0]["id"].str(), std::string_view("fireblast"));
        CHECK_EQ(q["moves"][0]["type"].str(), std::string_view("Fire"));
        CHECK_EQ(q["moves"][0]["category"].str(), std::string_view("Special"));
        CHECK_EQ(q["moves"][0]["basePower"].integer(), 110);
        CHECK_EQ(q["moves"][0]["accuracy"].integer(), 85);
        CHECK_EQ(q["moves"][0]["pp"].integer(), 8);
        CHECK(q["moves"][2]["accuracy"].is_bool());
        CHECK(!q["moves"][0]["desc"].str().empty());
        CHECK_EQ(q["canTera"].str(), std::string_view("Grass"));
        CHECK(!q["trapped"].boolean);
        CHECK_EQ(q["switches"].size(), 6u);
        CHECK_EQ(q["switches"][0]["slot"].integer(), 1);
        CHECK(q["switches"][0]["active"].boolean);
        CHECK_EQ(q["switches"][1]["name"].str(), std::string_view("Walking Wake"));
        CHECK_EQ(q["switches"][1]["hp"].integer(), 286);
        CHECK(!q["switches"][1]["fainted"].boolean);

        const auto menu = b.menu();
        CHECK(std::ranges::find(menu, "move 1  Fire Blast  Fire, Special, 110 power, 8/8 PP") != menu.end());
        CHECK(std::ranges::find(menu, "move 3  Toxic Spikes  Poison, Status, 32/32 PP") != menu.end());
        CHECK(std::ranges::find(menu, "switch 2  Walking Wake  100%") != menu.end());
        CHECK(std::ranges::none_of(menu, [](const std::string& l) {
            return l.starts_with("switch 1 ");
        }));

        const auto choice = [&](std::string_view typed) {
            std::string why;
            const auto c = b.parse_choice(typed, why);
            CHECK(c.has_value() != !why.empty());
            return c.value_or("");
        };
        CHECK_EQ(choice("1"), std::string("move 1"));
        CHECK_EQ(choice("move 2"), std::string("move 2"));
        CHECK_EQ(choice("Move Fire Blast"), std::string("move 1"));
        CHECK_EQ(choice("sludgewave"), std::string("move 2"));
        CHECK_EQ(choice("energy ball"), std::string("move 4"));
        CHECK_EQ(choice("move 1 tera"), std::string("move 1 terastallize"));
        CHECK_EQ(choice("tera 3"), std::string("move 3 terastallize"));
        CHECK_EQ(choice("switch 2"), std::string("switch 2"));
        CHECK_EQ(choice("switch walking wake"), std::string("switch 2"));
        CHECK_EQ(choice("sylveon"), std::string("switch 6"));
        CHECK_EQ(choice("switch 1"), std::string());
        CHECK_EQ(choice("switch 7"), std::string());
        CHECK_EQ(choice("move 5"), std::string());
        CHECK_EQ(choice("switch 2 tera"), std::string());
        CHECK_EQ(choice("splash"), std::string());
        CHECK_EQ(choice(""), std::string());

        b.chosen();
        CHECK_EQ(b.rqid(), 0);
        CHECK(parsed(b)["request"].is_null());
        CHECK(!b.parse_choice("1", error));
    });

    // Every request has an rqid of its own, even without one from the simulator.
    Battle all("p2");
    int last = 0;
    int requests = 0;
    for (const Message& m : load("battle-weather.log")) {
        if (m.stream != "p2") {
            continue;
        }
        all.feed(m.text);
        if (m.text.starts_with("|request|") && all.rqid() != 0) {
            CHECK(all.rqid() != last);
            last = all.rqid();
            ++requests;
            const auto state = parsed(all);
            const auto& q = state["request"];
            if (m.text.find("\"forceSwitch\":[true]") != std::string::npos) {
                CHECK_EQ(q["kind"].str(), std::string_view("switch"));
                CHECK_EQ(q["moves"].size(), 0u);
                std::string why;
                CHECK(!all.parse_choice("move 1", why));
                // The fainted one is still in, and cannot be chosen.
                const int active = state["sides"][1]["active"].integer();
                CHECK(!all.parse_choice(std::format("switch {}", active + 1), why));
            }
            if (m.text.find("\"trapped\":true") != std::string::npos) {
                CHECK(q["trapped"].boolean);
                std::string why;
                for (int slot = 1; slot <= 6; ++slot) {
                    CHECK(!all.parse_choice(std::format("switch {}", slot), why));
                }
            }
            if (m.text.find("\"disabled\":true") != std::string::npos) {
                for (const auto& move : q["moves"].items) {
                    std::string why;
                    const auto c = all.parse_choice(std::format("move {}", move["slot"].integer()), why);
                    CHECK_EQ(c.has_value(), !move["disabled"].boolean);
                }
            }
            all.chosen();
        } else if (m.text.starts_with("|request|")) {
            // wait: nothing to choose.
            CHECK(m.text.find("\"wait\":true") != std::string::npos);
        }
    }
    CHECK(requests > 20);
}

TEST(pokemon_error_reopens_the_request) {
    Battle b("p1");
    int rqid = 0;
    for (const Message& m : load("battle-error.log")) {
        if (m.stream != "p1") {
            continue;
        }
        if (m.text.starts_with("|error|")) {
            CHECK_EQ(b.rqid(), 0);
            b.feed(m.text);
            break;
        }
        b.feed(m.text);
        if (b.rqid() != 0) {
            rqid = b.rqid();
            b.chosen();
        }
    }
    CHECK(rqid > 0);
    CHECK_EQ(b.rqid(), rqid);
    CHECK(has_line(b, "[Invalid choice] Can't switch: You have to pass to a fainted Pokémon", LogLine::Kind::Error));
    const auto state = parsed(b);
    CHECK_EQ(state["error"].str(),
             std::string_view("[Invalid choice] Can't switch: You have to pass to a fainted Pokémon"));
    CHECK_EQ(state["request"]["kind"].str(), std::string_view("switch"));
    CHECK(state["request"]["reviving"].boolean);
    // Revival Blessing: only a fainted Pokémon can be chosen.
    std::string why;
    CHECK(!b.parse_choice("switch 2", why));
    CHECK(!why.empty());
    CHECK_EQ(b.parse_choice("switch 3", why).value_or(""), std::string("switch 3"));
    CHECK_EQ(b.parse_choice("hypno", why).value_or(""), std::string("switch 3"));
    const auto menu = b.menu();
    CHECK(std::ranges::any_of(menu, [](const std::string& l) {
        return l.starts_with("switch 3  Hypno");
    }));
    CHECK(std::ranges::none_of(menu, [](const std::string& l) {
        return l.starts_with("switch 2 ");
    }));
    b.chosen();
    CHECK_EQ(b.rqid(), 0);
    CHECK_EQ(parsed(b)["error"].str(), std::string_view());
}

TEST(pokemon_hand_written_lines) {
    // HP with a status, as a player sees its own.
    Battle p2("p2");
    p2.feed("|player|p1|Ash||\n|player|p2|Gary||\n|switch|p2a: Volbeat|Volbeat, L80, M, shiny|150/263 brn\n"
            "|switch|p1a: Annihilape|Annihilape, L76, F|100/100");
    auto state = parsed(p2);
    const auto* volbeat = mon(state, 1, "Volbeat");
    REQUIRE(volbeat);
    CHECK_EQ((*volbeat)["hp"].integer(), 150);
    CHECK_EQ((*volbeat)["maxhp"].integer(), 263);
    CHECK_EQ((*volbeat)["status"].str(), std::string_view("brn"));
    CHECK((*volbeat)["shiny"].boolean);
    CHECK((*volbeat)["exact"].boolean);
    CHECK_EQ((*volbeat)["level"].integer(), 80);
    CHECK(has_line(p2, "Go! Volbeat!"));
    CHECK(has_line(p2, "Ash sent out Annihilape!"));

    p2.feed("|move|p2a: Volbeat|Thunder Wave|p1a: Annihilape\n|-status|p1a: Annihilape|par\n"
            "|-boost|p2a: Volbeat|atk|1\n|-boost|p2a: Volbeat|atk|2\n|-boost|p2a: Volbeat|atk|3\n"
            "|-boost|p2a: Volbeat|atk|0\n|-unboost|p1a: Annihilape|spe|1\n|-unboost|p1a: Annihilape|spe|2\n"
            "|-unboost|p1a: Annihilape|spe|3\n|-unboost|p1a: Annihilape|def|0\n|-damage|p1a: Annihilape|66/100 par\n"
            "|-sidestart|p1: Ash|move: Stealth Rock\n|-sidestart|p2: Gary|Reflect\n|-sidestart|p1: Ash|Spikes\n"
            "|-sidestart|p1: Ash|Spikes\n|-fieldstart|move: Electric Terrain\n|-fieldstart|move: Trick Room|[of] p2a: "
            "Volbeat\n"
            "|-weather|RainDance\n|-crit|p1a: Annihilape\n|-supereffective|p1a: Annihilape\n");
    CHECK(has_line(p2, "Volbeat used Thunder Wave!", LogLine::Kind::Move));
    CHECK(has_line(p2, "The opposing Annihilape is paralyzed! It may be unable to move!"));
    CHECK(has_line(p2, "Volbeat's Attack rose!"));
    CHECK(has_line(p2, "Volbeat's Attack rose sharply!"));
    CHECK(has_line(p2, "Volbeat's Attack rose drastically!"));
    CHECK(has_line(p2, "Volbeat's Attack won't go any higher!"));
    CHECK(has_line(p2, "The opposing Annihilape's Speed fell!"));
    CHECK(has_line(p2, "The opposing Annihilape's Speed fell harshly!"));
    CHECK(has_line(p2, "The opposing Annihilape's Speed fell severely!"));
    CHECK(has_line(p2, "The opposing Annihilape's Defense won't go any lower!"));
    CHECK(has_line(p2, "(The opposing Annihilape lost 34% of its health!)", LogLine::Kind::Minor));
    CHECK(has_line(p2, "Pointed stones float in the air around the opposing team!"));
    CHECK(has_line(p2, "Reflect made your team stronger against physical moves!"));
    CHECK(has_line(p2, "An electric current ran across the battlefield!"));
    CHECK(has_line(p2, "Volbeat twisted the dimensions!"));
    CHECK(has_line(p2, "It started to rain!"));
    state = parsed(p2);
    CHECK_EQ((*mon(state, 1, "Volbeat"))["boosts"]["atk"].integer(), 6);
    CHECK_EQ((*mon(state, 0, "Annihilape"))["boosts"]["spe"].integer(), -6);
    CHECK_EQ((*mon(state, 0, "Annihilape"))["status"].str(), std::string_view("par"));
    CHECK_EQ((*mon(state, 0, "Annihilape"))["hp"].integer(), 66);
    CHECK(contains(state["sides"][0]["conditions"], "Stealth Rock"));
    CHECK(contains(state["sides"][0]["conditions"], "Spikes (2)"));
    CHECK(contains(state["sides"][1]["conditions"], "Reflect"));
    CHECK_EQ(state["field"]["terrain"].str(), std::string_view("Electric Terrain"));
    CHECK(contains(state["field"]["other"], "Trick Room"));
    CHECK_EQ(state["field"]["weather"].str(), std::string_view("Rain"));

    p2.feed("|-clearallboost\n|-fieldend|move: Electric Terrain\n|-fieldend|move: Trick Room\n"
            "|-sideend|p2: Gary|Reflect\n|-weather|none\n|-start|p1a: Annihilape|Substitute\n"
            "|-activate|p1a: Annihilape|Substitute|[damage]\n|-start|p1a: Annihilape|confusion\n"
            "|-item|p1a: Annihilape|Choice Scarf|[from] move: Trick\n|-ability|p1a: Annihilape|Defiant\n"
            "|-enditem|p2a: Volbeat|Sitrus Berry|[eat]\n|-heal|p1a: Annihilape|80/100 par|[from] item: Leftovers\n"
            "|cant|p1a: Annihilape|par\n|faint|p1a: Annihilape\n|-message|Something happened.\n|-hint|A hint.\n");
    state = parsed(p2);
    CHECK_EQ((*mon(state, 1, "Volbeat"))["boosts"].size(), 0u);
    CHECK_EQ(state["field"]["terrain"].str(), std::string_view());
    CHECK_EQ(state["field"]["other"].size(), 0u);
    CHECK_EQ(state["field"]["weather"].str(), std::string_view());
    CHECK(!contains(state["sides"][1]["conditions"], "Reflect"));
    CHECK(has_line(p2, "The electricity disappeared from the battlefield."));
    CHECK(has_line(p2, "The twisted dimensions returned to normal!"));
    CHECK(has_line(p2, "Your team's Reflect wore off!"));
    CHECK(has_line(p2, "The rain stopped."));
    CHECK(has_line(p2, "The opposing Annihilape put in a substitute!"));
    CHECK(has_line(p2, "The substitute took damage for the opposing Annihilape!"));
    CHECK(has_line(p2, "The opposing Annihilape became confused!"));
    CHECK(has_line(p2, "The opposing Annihilape obtained one Choice Scarf."));
    CHECK(has_line(p2, "[The opposing Annihilape's Defiant]"));
    CHECK(has_line(p2, "(Volbeat ate its Sitrus Berry!)"));
    CHECK(has_line(p2, "The opposing Annihilape restored a little HP using its Leftovers!"));
    CHECK(has_line(p2, "The opposing Annihilape is paralyzed! It can't move!"));
    CHECK(has_line(p2, "The opposing Annihilape fainted!"));
    CHECK(has_line(p2, "Something happened."));
    CHECK(has_line(p2, "(A hint.)", LogLine::Kind::Minor));
    const auto* ape = mon(state, 0, "Annihilape");
    REQUIRE(ape);
    CHECK_EQ((*ape)["item"].str(), std::string_view("Leftovers"));
    CHECK_EQ((*ape)["ability"].str(), std::string_view("Defiant"));
    CHECK_EQ((*ape)["status"].str(), std::string_view("fnt"));
    CHECK_EQ((*ape)["hp"].integer(), 0);
    CHECK_EQ((*ape)["volatiles"].size(), 0u);
    CHECK_EQ((*mon(state, 1, "Volbeat"))["item"].str(), std::string_view());

    // A spectator: p1's side is near, p2's "the opposing".
    Battle spectator("");
    spectator.feed(
        "|player|p1|Ash||\n|player|p2|Gary||\n|switch|p1a: Volbeat|Volbeat, L80|100/100\n"
        "|switch|p2a: Annihilape|Annihilape, L76|100/100\n|move|p2a: Annihilape|Close Combat|p1a: Volbeat\n"
        "|-damage|p1a: Volbeat|0 fnt\n|faint|p1a: Volbeat\n|-damage|p2a: Annihilape|90/100|[from] Stealth Rock\n");
    CHECK(has_line(spectator, "The opposing Annihilape used Close Combat!"));
    CHECK(has_line(spectator, "Volbeat fainted!"));
    CHECK(has_line(spectator, "(Volbeat lost 100% of its health!)"));
    CHECK(has_line(spectator, "Pointed stones dug into the opposing Annihilape!"));
    CHECK_EQ(spectator.rqid(), 0);

    // A tie, and the chat and notices of zchat.
    spectator.chat("Rex", "#ff0000", "gg");
    spectator.notice("The timer is on.");
    spectator.feed("|tie");
    CHECK(spectator.over());
    CHECK_EQ(spectator.winner(), std::string());
    CHECK(has_line(spectator, "Tie between Ash and Gary!", LogLine::Kind::Result));
    state = parsed(spectator);
    const auto& log = state["log"];
    bool chat = false;
    for (const auto& l : log.items) {
        if (l["k"].str() == "chat") {
            chat = true;
            CHECK_EQ(l["n"].str(), std::string_view("Rex"));
            CHECK_EQ(l["c"].str(), std::string_view("#ff0000"));
            CHECK_EQ(l["t"].str(), std::string_view("gg"));
        } else {
            CHECK(l.find("n") == nullptr);
        }
    }
    CHECK(chat);
}

TEST(pokemon_split_lines) {
    // The secret half of a |split| is for the side it names.
    const std::string message =
        "|split|p1\n|switch|p1a: Volbeat|Volbeat, L80|263/263\n|switch|p1a: Volbeat|Volbeat, L80|100/100";
    Battle p1("p1");
    p1.feed(message);
    const auto state1 = parsed(p1);
    CHECK_EQ((*mon(state1, 0, "Volbeat"))["maxhp"].integer(), 263);
    Battle p2("p2");
    p2.feed(message);
    const auto state2 = parsed(p2);
    CHECK_EQ((*mon(state2, 0, "Volbeat"))["maxhp"].integer(), 100);
    CHECK_EQ(std::ranges::count_if(p2.log(),
                                   [](const LogLine& l) {
                                       return l.text.find("Volbeat") != std::string::npos;
                                   }),
             1);
}

TEST(pokemon_illusion) {
    Battle b("p1");
    b.feed("|player|p1|Ash||\n|player|p2|Gary||\n|switch|p2a: Lugia|Lugia, L75|100/100\n"
           "|-damage|p2a: Lugia|60/100\n|replace|p2a: Zoroark|Zoroark-Hisui, L80, M|60/100\n|-end|p2a: "
           "Zoroark|Illusion\n");
    const auto state = parsed(b);
    CHECK_EQ(state["sides"][1]["team"].size(), 1u);
    const auto* zoroark = mon(state, 1, "Zoroark");
    REQUIRE(zoroark);
    CHECK_EQ((*zoroark)["species"].str(), std::string_view("Zoroark-Hisui"));
    CHECK_EQ((*zoroark)["hp"].integer(), 60);
    CHECK(has_line(b, "The opposing Zoroark's illusion wore off!"));
}

TEST(pokemon_malformed_input) {
    for (const char* side : {"p1", "p2", ""}) {
        Battle b {std::string(side)};
        for (const char* junk : {"",
                                 "|",
                                 "||||",
                                 "|switch|",
                                 "|switch|p3a: X|X|100/100",
                                 "|switch|p1a|",
                                 "|switch|p1a: X||",
                                 "|-damage|p1a: Nobody|abc",
                                 "|-damage|p1a: X|-5/100",
                                 "|turn|abc",
                                 "|turn|99999999999999999999",
                                 "|request|",
                                 "|request|{bad json",
                                 "|request|[1,2]",
                                 "|request|null",
                                 "|request|{\"active\":[{\"moves\":5}]}",
                                 "|request|{\"active\":[{\"moves\":[1,{\"move\":7}]}],\"side\":{\"id\":\"p1\","
                                 "\"pokemon\":[{\"ident\":5}]}}",
                                 "|request|{\"side\":{\"id\":\"p1\",\"pokemon\":[{\"ident\":\"p1: "
                                 "A\",\"details\":\"A\",\"condition\":\"x/y\"}]},\"forceSwitch\":[true]}",
                                 "|-boost|p1a: X|atk|999999",
                                 "|-sidestart|p9: Nobody|Spikes",
                                 "|-sidestart|p1|",
                                 "|-weather|",
                                 "|-start|p1a: X|",
                                 "|move|p1a: X",
                                 "|-activate|",
                                 "|-ability|p1a: X",
                                 "|detailschange|p1a: X|",
                                 "|zchat-types|p1a: X|",
                                 "|split|p1",
                                 "|split|",
                                 "|error|",
                                 "|win|",
                                 "|-swapboost|p1a: X|p1a: X|",
                                 "|-copyboost|p1a: X|p2a: Y",
                                 "|-hitcount|",
                                 "|-fail|",
                                 "|-immune|",
                                 "|-miss|",
                                 "|-heal|p1a: X|",
                                 "\x01\x02\xff\xfe",
                                 "|raw|<b>html</b>",
                                 "|c|~|hi",
                                 "no bar at all",
                                 "|-message|\x1b[31mred",
                                 "|unknowncommand|a|b|[from] ability: Mystery|[of] p1a: X",
                                 "|-damage|p1a: X|50/100|[from]|[of]",
                                 "|switch|p1a: X|X, L|50"}) {
            b.feed(junk);
        }
        // The same name for both a Pokémon and a side changes nothing.
        b.feed("|switch|p1a: Ash|Ash, L5|10/10\n|player|p1|Ash||\n|-sidestart|p1: Ash|Spikes");
        std::string deep(100000, '[');
        b.feed("|request|" + deep);
        b.feed(std::string("|request|{\"wait\":true}"));
        const auto state = zchat::json::parse(b.json(""));
        CHECK(state.has_value());
        for (const LogLine& l : b.log()) {
            for (const char c : l.text) {
                CHECK(static_cast<unsigned char>(c) >= 0x20);
            }
        }
        std::string why;
        CHECK(!b.parse_choice("move 1", why) || std::string(side) == "p1");
    }
}

TEST(json_parse) {
    using zchat::json::parse;
    const auto v =
        parse(R"( {"a": [1, -2.5e2, true, false, null, "x\"\\\/\b\f\n\r\t\u00e9\ud83d\ude00"], "b": {}, "a2": []} )");
    REQUIRE(v.has_value());
    CHECK_EQ(v->size(), 3u);
    CHECK_EQ((*v)["a"][0].integer(), 1);
    CHECK_EQ((*v)["a"][1].number, -250.0);
    CHECK((*v)["a"][2].boolean);
    CHECK((*v)["a"][3].is_bool() && !(*v)["a"][3].boolean);
    CHECK((*v)["a"][4].is_null());
    CHECK_EQ((*v)["a"][5].str(), std::string_view("x\"\\/\b\f\n\r\t\xC3\xA9\xF0\x9F\x98\x80"));
    CHECK((*v)["b"].is_object());
    CHECK((*v)["missing"]["deeper"][3].is_null());
    CHECK_EQ((*v)["a"][99].integer(7), 7);
    // A lone surrogate is not a code point.
    CHECK_EQ(parse(R"("\ud83d")")->str(), std::string_view("\xEF\xBF\xBD"));
    CHECK_EQ(parse(R"("\ude00x")")->str(), std::string_view("\xEF\xBF\xBDx"));
    for (const char* bad :
         {"",        " ",         "{",        "}",     "[1,]",       "{\"a\":}", "{\"a\" 1}", "{a:1}",
          "01",      "1.",        ".5",       "-",     "1e",         "tru",      "nul",       "\"abc",
          "\"\\x\"", "\"\\u12\"", "\"a\nb\"", "[1] 2", "{\"a\":1,}", "+1",       "NaN",       "Infinity"}) {
        CHECK(!parse(bad).has_value());
    }
    CHECK(!parse(std::string(1000, '[') + std::string(1000, ']')).has_value());
    CHECK(parse(std::string(10, '[') + std::string(10, ']')).has_value());
    CHECK(parse("1e400").has_value());
}

TEST(json_quote) {
    using zchat::json::quote;
    CHECK_EQ(quote("a\"b\\c\n\t\x01"), std::string("\"a\\\"b\\\\c\\n\\t\\u0001\""));
    CHECK_EQ(quote("Pok\xC3\xA9mon"), std::string("\"Pok\xC3\xA9mon\""));
    CHECK_EQ(quote("\xff"), std::string("\"\xEF\xBF\xBD\""));
    CHECK_EQ(quote("\xE2\x80\xA8"), std::string("\"\\u2028\""));
    const auto back = zchat::json::parse(quote("any \"text\" \\ \x7f with\r\nlines"));
    REQUIRE(back.has_value());
    CHECK_EQ(back->str(), std::string_view("any \"text\" \\ \x7f with\r\nlines"));
}
