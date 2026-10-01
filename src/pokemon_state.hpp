#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zchat::pokemon {

// A Pokémon Showdown battle as one of its viewers sees it, put together from the battle's messages in Showdown's
// protocol (see PROTOCOL.md and sim/SIM-PROTOCOL.md of pokemon-showdown, and src/pokemon/bridge.js for what zchat's
// bridge adds to them). It knows nothing of the network nor of the simulator: it is fed messages and tells what they
// mean, for the battle window (json()) and the terminal (log(), menu()).

// A line of the battle log, as shown: plain text (UTF-8, no ANSI escapes).
struct LogLine {
    enum class Kind {
        Turn,   // "Turn 3"
        Move,   // "Great Tusk used Headlong Rush!"
        Text,   // what happens: "It's super effective!", "The opposing Gholdengo fainted!"
        Minor,  // details: "(Great Tusk lost 34% of its health!)"
        Chat,   // something said in the battle's chat: name and color are who said it
        Notice, // from zchat, not the battle: "The timer is on."
        Error,  // a choice the simulator refused
        Result, // "Ash won the battle!"
    };
    Kind kind = Kind::Text;
    std::string text;
    std::string name;
    // "#rrggbb"
    std::string color;
};

class Battle {
public:
    // side is "p1" or "p2" for what a player sees (their own stream), or empty for a spectator.
    explicit Battle(std::string side = {});

    // One message of the battle's stream: Showdown protocol lines separated by '\n'.
    void feed(std::string_view message);
    // Something said in the battle's chat, and a line of zchat's own, added to the log.
    void chat(std::string_view name, std::string_view color, std::string_view text);
    void notice(std::string_view text);

    const std::vector<LogLine>& log() const {
        return log_;
    }

    // "p1", "p2", or empty for a spectator.
    const std::string& side() const {
        return side_;
    }
    // The name of a player ("p1" or "p2") as the battle has it, empty before it is known.
    std::string player_name(std::string_view side) const;
    int turn() const {
        return turn_;
    }
    // The generation of the battle, 1 to 9 (9 until the battle says).
    int gen() const {
        return gen_;
    }

    // The request waiting for our choice: its rqid, or 0 when there is none (a spectator, the opponent is still
    // choosing, or the battle is over). A request is answered once: chosen() makes this 0 until the next one, and an
    // |error| from the simulator brings it back.
    int rqid() const;
    void chosen();
    // The choice is taken back, before the other player chose (the turn has not been played): the request that was
    // answered waits for a choice again.
    void unchoose();

    bool over() const {
        return over_;
    }
    // The winner's name; empty when it is not over, or a tie.
    const std::string& winner() const {
        return winner_;
    }

    // For the terminal: the choices of the request waiting, one per line, like
    //   "move 1  Headlong Rush  Ground, Physical, 120 power, 8/8 PP"
    //   "switch 2  Gholdengo  71%"
    // and "move 1 tera" when Terastallizing is possible. Empty when there is no request.
    std::vector<std::string> menu() const;

    // Turns a choice typed in the terminal or sent by the window into Showdown's: "1".."4" or "move 2" (a move by
    // slot), "move earthquake" or "earthquake" (by name), "switch 3" or "switch gholdengo", with "tera" added to
    // Terastallize ("move 1 tera", "tera 1"). Returns nullopt, with why in error, when the request waiting does not
    // allow it (there is none, the move is disabled, the Pokémon has fainted or is already in...).
    std::optional<std::string> parse_choice(std::string_view typed, std::string& error) const;

    // The state, for the battle window (see the Pokémon battle in src/ui/index.html): a JSON object whose first
    // members are extra (other JSON members, without braces; may be empty), then:
    //   "you": "p1" | "p2" | ""                   the side we are on, "" for a spectator
    //   "gen": number                             the generation, 1 to 9 (from |gen|; 9 until it is known)
    //   "turn": number
    //   "over": bool, "winner": "name"            the winner is "" for a tie, or while it is not over
    //   "sides": [side, side]                     p1 then p2, each:
    //       {"id": "p1", "name": "Ash", "teamSize": 6, "active": index in team or -1,
    //        "team": [mon...],                    ours: the whole team, as the request has it; the opponent's: the
    //                                             ones seen so far, in the order they appeared
    //        "conditions": ["Stealth Rock", "Spikes (2)", "Reflect"]}
    //       mon: {"name": nickname, "species": "Great Tusk", "level": 82, "gender": "M" | "F" | "", "shiny": bool,
    //             "hp": number, "maxhp": number, "exact": bool (true: real HP, ours; false: hp is out of maxhp 100),
    //             "status": "brn" | "par" | "slp" | "frz" | "psn" | "tox" | "fnt" | "",
    //             "types": ["Ground", "Fighting"], "tera": "Steel" (the type it has Terastallized into) or "",
    //             "teraType": "Steel" (ours only, the type it can Terastallize into) or "",
    //             "boosts": {"atk": 1, ...}, "volatiles": ["Substitute", "Confusion"],
    //             "item": "Leftovers" | "", "ability": "Prankster" | "", "moves": ["Headlong Rush", ...] (all of
    //             ours; the opponent's as they are used), "stats": {"atk": 183, ...} (ours only, else {}),
    //             "dynamaxed": bool (it is Dynamaxed now), "mega": bool (it is in a Mega forme)}
    //   "field": {"weather": "Rain" | "", "terrain": "Electric Terrain" | "", "other": ["Trick Room", ...]}
    //   "request": null, or the request waiting (see rqid()):
    //       {"rqid": 5, "kind": "move" | "switch",   switch: a forced switch, after a faint or U-turn
    //        "moves": [{"slot": 1, "name": "Thunder Wave", "id": "thunderwave", "pp": 32, "maxpp": 32 (both null when
    //                   not counted: the only thing left to do, like Recharge or a locked Outrage, which is not
    //                   out of PP),
    //                   "type": "Electric", "category": "Status", "basePower": 0, "accuracy": 90 | true,
    //                   "desc": "...", "disabled": bool,
    //                   "zmove": null | gimmick move,      its Z-Move, when it can be one now
    //                   "maxMove": null | gimmick move}...]  its Max Move, when it can Dynamax or is Dynamaxed
    //                                                        (empty for a switch)
    //            gimmick move: {"name": "Breakneck Blitz", "type": "Normal", "category": "Physical",
    //                           "basePower": 175, "accuracy": true | number, "desc": "..."}
    //        "canTera": "Steel" | "", "trapped": bool,
    //        "canMega": bool, "canUltraBurst": bool, "canDynamax": bool,   what may go with a move this turn
    //        "dynamaxed": bool                  the active one is Dynamaxed: its moves are used as their Max Moves
    //        "reviving": bool                   Revival Blessing: a switch to a fainted one, which comes back
    //        "switches": [{"slot": 2, "name": "Orthworm", "species": "Orthworm", "hp": 266, "maxhp": 266,
    //                      "status": "", "fainted": bool, "active": bool}...]   (the whole team, by slot)}
    //   "error": the last choice refused, while its request is waiting again; else ""
    //   "log": [{"k": "turn" | "move" | "text" | "minor" | "chat" | "notice" | "error" | "result",
    //            "t": text, "n": name, "c": color}...]   (n and c only for chat)
    std::string json(std::string_view extra) const;

private:
    struct Mon {
        std::string ident; // "p1: Volbeat", without the position
        std::string name;
        std::string species;
        int level = 100;
        std::string gender;
        bool shiny = false;
        int hp = 100;
        int maxhp = 100;
        bool exact = false;
        std::string status;
        std::vector<std::string> types;
        // The types a move or ability gave it (Soak, Double Shock...) until it switches out; empty when none did.
        std::vector<std::string> changed_types;
        std::string tera;
        std::string tera_type;
        std::map<std::string, int> boosts;
        std::vector<std::string> volatiles;
        std::string item;
        std::string ability;
        std::vector<std::string> moves;
        std::map<std::string, int> stats;
        // Seen for the first time when it last switched in: if Illusion is then broken, it was never there.
        bool fresh = false;
    };
    struct Condition {
        std::string name;
        int layers = 1;
    };
    struct Side {
        std::string name;
        int team_size = 6;
        int active = -1;
        std::vector<Mon> team;
        std::vector<Condition> conditions;
        // The move its active Pokémon used last this turn (an id, like "uturn"), for the words of a switch after it.
        std::string last_move;
    };
    // A Z-Move or Max Move a move would be.
    struct Gimmick {
        std::string name;
        std::string type;
        std::string category;
        int base_power = 0;
        int accuracy = 0; // 0 when it never misses
        std::string desc;
    };
    struct Move {
        std::string name;
        std::string id;
        int pp = 0;
        int maxpp = 0;
        // Whether its PP are counted: not for the only thing left to do (Recharge, a locked Outrage, Struggle...).
        bool counted = true;
        std::string type;
        std::string category;
        int base_power = 0;
        // 0 when it never misses (true in the request).
        int accuracy = 0;
        std::string desc;
        bool disabled = false;
        std::optional<Gimmick> zmove;
        std::optional<Gimmick> max_move;
    };
    // The request waiting for our choice, as the simulator sent it.
    struct Request {
        int rqid = 0;
        std::string kind; // "move", "switch" or "team" (Team Preview)
        std::vector<Move> moves;
        std::string can_tera;
        bool can_mega = false;
        bool can_ultra = false;
        bool can_dynamax = false;
        bool dynamaxed = false;
        bool trapped = false;
        // Revival Blessing: the switch is to one of the fainted Pokémon, which comes back.
        bool reviving = false;
    };
    // A protocol line split up as Showdown's client does it: the command and its arguments, then the [key] value
    // tags at the end.
    struct Line;

    void line(std::string_view line);
    void request(std::string_view json);
    // What the line means, in Showdown's words (lines separated by '\n'), from the state before it changes it.
    std::string describe(const Line& line) const;
    // Adds what describe() said to the log.
    void say(const Line& line, std::string_view text);
    // Updates the state for the line.
    void apply(const Line& line);

    // The side of an ident ("p1a: Volbeat" is side 0), or -1 when it is not one.
    static int side_index(std::string_view ident);
    Mon* find(std::string_view ident);
    const Mon* find(std::string_view ident) const;
    Mon* active(int side);
    // The Pokémon an ident names, taken in as the active one of its side by a switch, drag or replace.
    void switch_in(const Line& line);
    void set_hp(Mon& mon, std::string_view condition) const;
    // An item or ability ("item: Leftovers") seen on a Pokémon.
    void reveal(std::string_view effect, std::string_view holder);
    // The words of the Showdown text table: "[POKEMON]" is "the opposing Volbeat", "[TEAM]" "your team"...
    std::string pokemon_text(std::string_view ident) const;
    std::string team_text(int side) const;
    bool near(int side) const;
    bool own(int side) const;
    std::string trainer(int side) const;

    std::string side_;
    int gen_ = 9;
    Side sides_[2];
    int turn_ = 0;
    bool over_ = false;
    std::string winner_;
    // The weather as protocol has it ("RainDance", for its texts) and as shown ("Rain").
    std::string weather_id_;
    std::string weather_;
    std::string terrain_;
    std::vector<std::string> field_other_;
    std::vector<LogLine> log_;
    // The request waiting, whether it was answered, and the count of requests, for an rqid when they have none.
    std::optional<Request> request_;
    bool answered_ = true;
    int requests_ = 0;
    std::string error_;
    // A |split| line was seen: the next line is the exact one for the side it named, the one after for everyone.
    int split_ = 0;
    bool split_ours_ = false;
};

} // namespace zchat::pokemon
