// Pokémon: battles of Pokémon Showdown's Random Battles, of any generation from 1 to 9, between two people in the
// chat, with /game pokemon challenge NAME [genN], which anybody else can watch. The battles are Showdown's own: its simulator runs with
// Node.js (see pokemon::Engine), on the zchat of one of the two players, the referee, which tells the others what
// happens. zchat works without Node.js: only the battles need it, and only on one of the two players' computers.
//
// Every peer that hears of a battle keeps what it knows of it; the players and those watching also keep the battle
// itself (pokemon::Battle), shown in a window of its own (see Screen::show_game()) or in the terminal. As Game
// packets:
//   pokemon challenge B NODE FORMAT NAME   asks NAME (the rest of the text) to battle; B is a random hex number
//                                          naming the battle, NODE is 1 when we have Node.js, else 0, FORMAT is
//                                          Showdown's (gen1randombattle to gen9randombattle; older versions did
//                                          not send it, and their battles are gen9randombattle)
//   pokemon accept B NODE | decline B      the answer, from who was challenged
//   pokemon cancel B REASON                the challenger gives up on it (nobody has Node.js...)
//   pokemon start B REFEREE P1 P2 N1 N2 FORMAT   the challenger starts it: ids in hex, and the names of P1
//                                          (the challenger) and P2, with %-escapes for spaces
// then the referee sends what the simulator says, in three streams of numbered messages, each split in parts that
// fit in a packet:
//   pokemon log B SEQ PART/COUNT TEXT      what everyone may see, for those watching
//   pokemon priv B SIDE SEQ PART/COUNT TEXT  what only player SIDE (p1 or p2) may see: their own stream, which has
//                                          everything they are shown (another zchat could read it: battles are
//                                          for fun, not for money)
//   pokemon head B LOG P1 P2 TIMER L1 L2 OVER ID1 ID2 N1 N2 FORMAT   every second or two: how many messages each
//                                          stream
//                                          has, whether the timer is on and the seconds each player has left to
//                                          choose (-1: not choosing), how it ended (0 while it goes on, p1, p2,
//                                          tie or none) and who plays it; it also tells the referee is still here
//   pokemon end B OVER REASON              it is over; REASON says why when nobody won
// and the others, to the referee:
//   pokemon ack B SIDE SEQ                 a player has its stream up to SEQ; sent every few seconds too, to tell
//                                          they are still here
//   pokemon want B STREAM FROM             someone missed messages of a stream (log, p1 or p2): sent again from
//                                          FROM, a few at a time (a player's stream only to them)
//   pokemon do B N ACTION                  a player's N-th action: "choose RQID CHOICE" (CHOICE as Showdown takes
//                                          it; choosing again replaces it, until the other player chose too),
//                                          "undo RQID" (takes the choice back, while the other has not chosen),
//                                          "timer" (turns the timer on or off) or "forfeit"; done in order, and sent
//                                          again until
//   pokemon done B SIDE N [RESULT]         the referee has done it: for an undo, RESULT is ok, or late when the
//                                          other player had chosen too (the turn is being played)
// and from anybody in the battle (players and those watching):
//   pokemon say B TEXT                     something said in the battle's chat

#include "game.hpp"

#include "color.hpp"
#include "pokemon_engine.hpp"
#include "pokemon_link.hpp"
#include "pokemon_state.hpp"
#include "text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <vector>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    using pokemon::link::decode_field;
    using pokemon::link::encode_field;
    using pokemon::link::unescape;
    using clock = std::chrono::steady_clock;

    constexpr std::string_view game_name = "pokemon";
    constexpr auto challenge_time = 60s;
    // The referee's heads: more often while the timer runs, for the seconds left.
    constexpr auto head_interval = 2s;
    constexpr auto head_interval_timer = 1s;
    // Messages of a player's stream not acknowledged are sent again so often, and actions not done.
    constexpr auto resend_interval = 1s;
    constexpr auto ack_interval = 2s;
    constexpr auto want_interval = 1s;
    // Without a head for so long, the referee is gone; without a word from a player, they lose.
    constexpr auto referee_silence = 20s;
    constexpr auto player_silence = 60s;
    // The timer: seconds to choose, and when the player is told how many are left.
    constexpr int turn_seconds = 150;
    constexpr std::array<int, 2> warnings = {30, 10};
    // A battle over is still sent again to whoever missed its end for so long, and remembered for longer.
    constexpr auto keep_referee = 60s;
    constexpr auto keep_battle = 10min;
    // A Game packet holds 1000 bytes of text: the parts of a message, with what comes before them.
    constexpr std::size_t part_bytes = 900;
    constexpr std::size_t max_resend = 16;
    // Parts of a message that never all came are dropped after so long (they are asked for again).
    constexpr auto partial_timeout = 30s;
    constexpr std::size_t max_said_bytes = 300;

    std::optional<std::uint64_t> parse_number(std::string_view s, int base = 10) {
        std::uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size()) {
            return std::nullopt;
        }
        return value;
    }

    std::optional<int> parse_int(std::string_view s) {
        int value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size()) {
            return std::nullopt;
        }
        return value;
    }

    // Takes the next word of s, up to a space, and removes it with the space.
    std::string_view next_field(std::string_view& s) {
        const auto space = s.find(' ');
        const std::string_view field = s.substr(0, space);
        s.remove_prefix(space == std::string_view::npos ? s.size() : space + 1);
        return field;
    }

    std::string lowercase(std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](char c) {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        });
        return out;
    }

    std::string trim(std::string_view s) {
        while (!s.empty() && s.front() == ' ') {
            s.remove_prefix(1);
        }
        while (!s.empty() && s.back() == ' ') {
            s.remove_suffix(1);
        }
        return std::string(s);
    }

    // A name as typed: the window's '@' list types it after an '@', which is not part of it.
    std::string person(std::string_view s) {
        std::string out = trim(s);
        return out.starts_with('@') ? trim(std::string_view(out).substr(1)) : out;
    }

    std::string json(std::string_view s) {
        std::string out = "\"";
        for (const char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
        return out + "\"";
    }

    std::string hex_color(std::uint64_t id) {
        const Color c = color_of_id(id);
        return std::format("#{:02x}{:02x}{:02x}", c.r, c.g, c.b);
    }

    // A name as the simulator gets it: without what its protocol uses to separate things.
    std::string battle_name(std::string_view name) {
        std::string out;
        for (const char c : name) {
            out += c == '|' || c == ',' || c == '[' || c == ']' ? ' ' : c;
        }
        return trim(out);
    }

    int side_index(std::string_view side) {
        return side == "p1" ? 0 : side == "p2" ? 1 : -1;
    }

    std::string_view side_name(int index) {
        return index == 0 ? "p1" : "p2";
    }

    // Showdown's random battles, one per generation: "gen3randombattle" as the packets carry it, and as it is shown.
    constexpr int default_gen = 9;

    std::string format_id(int gen) {
        return std::format("gen{}randombattle", gen);
    }

    std::string format_name(int gen) {
        return std::format("Gen {} Random Battle", gen);
    }

    // The generation of a format, or 0 when it is not one.
    int format_gen(std::string_view id) {
        if (id.size() == 16 && id.starts_with("gen") && id.ends_with("randombattle") && id[3] >= '1' && id[3] <= '9') {
            return id[3] - '0';
        }
        return 0;
    }

    // The generation typed, as "gen3" (or "Gen3"), or 0 when it is not one.
    int typed_gen(std::string_view word) {
        if (word.size() == 4 && (word[0] == 'g' || word[0] == 'G') && (word[1] == 'e' || word[1] == 'E') &&
            (word[2] == 'n' || word[2] == 'N') && word[3] >= '1' && word[3] <= '9') {
            return word[3] - '0';
        }
        return 0;
    }

    // What the bridge prints, kept for the chat thread (see Pokemon::tick()), as it comes on a thread of its own.
    struct Inbox {
        std::mutex mutex;
        std::deque<std::string> lines;
        std::optional<std::pair<int, std::string>> exit;
    };

    class Pokemon final : public Game {
    public:
        Pokemon(Chat& chat, Screen& terminal, Games& games) :
            chat_(chat),
            terminal_(terminal),
            games_(games),
            rng_(std::random_device {}()) {
        }

        ~Pokemon() override {
            // The bridges first: they must stop before what they report to is gone.
            for (auto& [id, b] : battles_) {
                if (b.referee) {
                    b.referee->child.reset();
                }
            }
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "Pokémon Showdown's random battles against someone in the chat (needs Node.js on one of you)";
        }

        std::vector<Help> help() const override {
            return {
                {"", "show your battle, or the ones going on"},
                {"challenge NAME [genN]", "challenge someone to a battle, of Gen 9 or gen1 to gen9"},
                {"accept [NAME]", "accept a challenge (the last one, without a name)"},
                {"decline [NAME]", "decline a challenge"},
                {"move N [tera|mega|ultra|z|max]", "use move N, with Terastallization, Mega Evolution..."},
                {"switch N", "switch to Pokémon N"},
                {"cancel", "take your choice back, until the other player chooses"},
                {"say TEXT", "say something in the battle's chat"},
                {"timer", "turn the timer on or off"},
                {"forfeit", "give up the battle"},
                {"watch NAME", "watch someone's battle"},
                {"unwatch", "stop watching it"},
                {"update", "update Pokémon Showdown (needs Node.js)"},
            };
        }

        bool keeps_case() const override {
            return true;
        }

        void start() override {
            if (Battle* b = own_battle()) {
                show(*b, true);
                return;
            }
            chat_.game_notice(
                "⚔ Pokémon battles, as on Pokémon Showdown (Random Battles): /game pokemon challenge NAME "
                "challenges someone in the chat, who answers with /game pokemon accept.");
            chat_.game_notice("   Gen 9 unless you say another: /game pokemon challenge NAME gen3 (gen1 to gen9).");
            chat_.game_notice(
                "   One of you needs Node.js (nodejs.org): Pokémon Showdown is installed with it, the first "
                "time, which takes a minute.");
            bool any = false;
            for (const auto& [id, b] : battles_) {
                if (!b.over) {
                    chat_.game_notice(std::format("   Going on: {} vs {} ({}), /game pokemon watch {} to watch it",
                                                  b.names[0], b.names[1], format_name(b.gen), b.names[0]));
                    any = true;
                }
            }
            if (!any) {
                chat_.game_notice("   No battle is going on now.");
            }
        }

        bool command(std::string_view args) override {
            std::string_view rest = args;
            const std::string verb = lowercase(next_field(rest));
            const std::string arg = trim(rest);
            if (verb == "challenge" || verb == "c") {
                challenge(arg);
            } else if (verb == "accept") {
                accept(person(arg));
            } else if (verb == "decline") {
                decline(person(arg));
            } else if (verb == "move" || verb == "switch" || verb == "tera" ||
                       (!verb.empty() && verb.find_first_not_of("0123456789") == std::string::npos)) {
                choose(std::string(args));
            } else if (verb == "say") {
                say(arg);
            } else if (verb == "cancel" || verb == "undo") {
                cancel();
            } else if (verb == "timer") {
                act("timer");
            } else if (verb == "forfeit") {
                act("forfeit");
            } else if (verb == "watch") {
                watch(person(arg));
            } else if (verb == "unwatch") {
                unwatch();
            } else if (verb == "update") {
                update();
            } else {
                return false;
            }
            return true;
        }

        void receive(std::uint64_t sender, std::string_view name, std::string_view text) override {
            const std::string_view event = next_field(text);
            const auto id = parse_number(next_field(text), 16);
            if (!id || *id == 0) {
                return;
            }
            if (event == "challenge") {
                receive_challenge(sender, name, *id, text);
            } else if (event == "accept") {
                receive_accept(sender, name, *id, text);
            } else if (event == "decline") {
                receive_decline(sender, name, *id);
            } else if (event == "cancel") {
                std::erase_if(challenges_, [&](const Challenge& c) {
                    if (c.battle == *id && !c.ours && c.from == sender) {
                        chat_.game_notice(std::format("⚔ {} called off the battle: {}",
                                                      chat_.colored_name(sender, name), text::sanitize(text, 200)));
                        return true;
                    }
                    return false;
                });
            } else if (event == "start") {
                receive_start(sender, *id, text);
            } else if (const auto it = battles_.find(*id); it != battles_.end()) {
                receive_battle(it->second, sender, name, event, text);
            } else if (event == "head") {
                receive_head(sender, *id, text);
            }
        }

        void message(std::uint64_t, std::string_view, std::string_view) override {
        }

        void tick() override {
            const auto now = clock::now();
            std::erase_if(challenges_, [&](const Challenge& c) {
                if (now - c.at < challenge_time) {
                    return false;
                }
                if (c.ours && !c.accepted) {
                    chat_.game_notice(std::format("⚔ {} did not answer your challenge.", c.colored_name));
                }
                return true;
            });
            show_challenges();
            for (auto& [id, b] : battles_) {
                if (b.referee) {
                    referee_tick(b);
                }
                if (b.view) {
                    viewer_tick(b);
                } else if (!b.referee && !b.over && now - b.last_head > referee_silence) {
                    // Gone without a word: not to be watched anymore.
                    b.over = true;
                    b.outcome = "none";
                    b.ended = now;
                }
            }
            if (updating_ && engine_.status() != pokemon::Engine::Status::Installing) {
                updating_ = false;
                chat_.game_notice(engine_.status() == pokemon::Engine::Status::Ready
                                      ? std::string("📦 Pokémon Showdown is up to date.")
                                      : std::format("Pokémon Showdown could not be updated: {}", engine_.failure()));
            }
            std::erase_if(battles_, [&](const auto& entry) {
                const Battle& b = entry.second;
                return b.over && !b.referee && now - b.ended > keep_battle && !(b.view && b.window_open);
            });
        }

    private:
        struct Challenge {
            std::uint64_t battle = 0;
            // Who challenged, or who we challenged.
            std::uint64_t from = 0;
            std::string name;
            std::string colored_name;
            bool ours = false;
            bool node = false;
            bool accepted = false;
            clock::time_point at;
            int gen = default_gen;
        };

        // A stream of the referee's: its messages (as the bridge printed them), and for a player's, how many they
        // acknowledged.
        struct Outbound {
            std::vector<std::string> messages;
            std::uint64_t acked = 0;
            clock::time_point resent;
        };

        struct Referee {
            std::unique_ptr<process::Child> child;
            std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
            bool running = false;
            bool announced_install = false;
            // log, p1, p2
            std::array<Outbound, 3> streams;
            // Each player's request waiting for their choice, since when, and whether they were told the time.
            std::array<int, 2> rqid {0, 0};
            std::array<bool, 2> waiting {false, false};
            std::array<clock::time_point, 2> asked;
            std::array<std::set<int>, 2> warned;
            // Each player's actions are done in order: the next one, and those that came before it.
            std::array<std::uint64_t, 2> next_action {1, 1};
            std::array<std::map<std::uint64_t, std::string>, 2> early_actions;
            // What the last ones did ("ok" or "late" for an undo, else empty), for the dones sent again.
            std::array<std::map<std::uint64_t, std::string>, 2> results;
            std::array<clock::time_point, 2> heard;
            bool timer = false;
            clock::time_point last_head;
            std::map<std::string, clock::time_point> wants;
        };

        struct Action {
            std::uint64_t n = 0;
            std::string text;
            clock::time_point sent;
        };

        // A battle we heard of: who plays it, and, when we are in it, the battle itself.
        struct Battle {
            std::uint64_t id = 0;
            std::uint64_t referee_id = 0;
            std::array<std::uint64_t, 2> ids {0, 0};
            std::array<std::string, 2> names;
            int gen = default_gen;
            clock::time_point last_head = clock::now();
            bool over = false;
            // p1, p2, tie or none; and why, for none.
            std::string outcome;
            std::string reason;
            clock::time_point ended;

            // Ours: the side we play, or empty when we watch.
            std::optional<pokemon::Battle> view;
            std::string side;
            pokemon::link::Inbound in {partial_timeout};
            bool ack_due = false;
            clock::time_point last_ack;
            clock::time_point last_want;
            std::uint64_t head_count = 0;
            std::deque<Action> actions;
            std::uint64_t next_action = 1;
            // We chose, for the request rqid, what label says ("Earthquake", "Gholdengo"): it can be taken back
            // until the turn is played.
            bool chose = false;
            int chose_rqid = 0;
            std::string chose_label;
            // Taking it back, until the referee says whether it could (the other player may have chosen already).
            bool cancelling = false;
            std::uint64_t cancel_n = 0;
            // The timer, from the last head: whether it is on and our seconds left then.
            bool timer = false;
            int left = -1;
            clock::time_point left_at;
            // Shown in a window, or else printed: how many lines of the log, and the last request shown.
            bool window = false;
            bool window_open = true;
            std::size_t printed = 0;
            int menu_rqid = 0;
            int last_left_shown = -2;

            std::unique_ptr<Referee> referee;
        };

        // --- Challenges

        // /game pokemon challenge NAME [genN]: the generation may come before the name too.
        void challenge(const std::string& typed) {
            std::string who = typed;
            int gen = default_gen;
            if (const auto space = who.rfind(' '); space != std::string::npos && typed_gen(who.substr(space + 1))) {
                gen = typed_gen(who.substr(space + 1));
                who = trim(who.substr(0, space));
            } else if (const auto first = who.find(' '); first != std::string::npos && typed_gen(who.substr(0, first))) {
                gen = typed_gen(who.substr(0, first));
                who = trim(who.substr(first + 1));
            } else if (typed_gen(who)) {
                chat_.game_notice("Who? /game pokemon challenge NAME gen3 (/who lists the people in the chat).");
                return;
            }
            who = person(who);
            if (who.empty()) {
                chat_.game_notice("Who? /game pokemon challenge NAME (/who lists the people in the chat).");
                return;
            }
            if (own_battle()) {
                chat_.game_notice("Finish your battle first (or /game pokemon forfeit).");
                return;
            }
            std::string target;
            // Their names as they are (peers() has them colored).
            for (const auto& peer : chat_.mentionable()) {
                if (lowercase(peer.name) == lowercase(who)) {
                    target = peer.name;
                }
            }
            if (target.empty()) {
                chat_.game_notice(
                    std::format("Nobody called {} is in the chat (/who lists them).", text::sanitize(who, 48)));
                return;
            }
            const bool node = engine_.has_node();
            std::uint64_t battle = 0;
            do {
                battle = rng_();
            } while (battle == 0);
            challenges_.push_back({battle, 0, target, target, true, node, false, clock::now(), gen});
            send(std::format("challenge {:x} {} {} {}", battle, node ? 1 : 0, format_id(gen), target));
            chat_.game_notice(std::format("⚔ You challenge {} to a Pokémon battle ({}): waiting for an answer…", target,
                                          format_name(gen)));
            if (!node) {
                chat_.game_notice(
                    "   You do not have Node.js: the battle can only happen if they have it (or install it from "
                    "nodejs.org).");
            }
        }

        void receive_challenge(std::uint64_t sender, std::string_view name, std::uint64_t battle,
                               std::string_view text) {
            const std::string_view node = next_field(text);
            int gen = default_gen;
            std::string_view rest = text;
            if (const int g = format_gen(next_field(rest))) {
                gen = g;
                text = rest;
            }
            if (lowercase(trim(text)) != lowercase(chat_.name()) ||
                std::ranges::any_of(challenges_, [&](const Challenge& c) {
                    return c.battle == battle;
                })) {
                return;
            }
            const std::string colored = chat_.colored_name(sender, name);
            challenges_.push_back(
                {battle, sender, std::string(name), colored, false, node == "1", false, clock::now(), gen});
            chat_.game_notice(std::format("⚔ {} challenges you to a Pokémon battle ({})! /game pokemon accept, or "
                                          "/game pokemon decline",
                                          colored, format_name(gen)));
            terminal_.bell();
        }

        // The challenges to us waiting for an answer, for the window to show them with buttons to accept or decline
        // (see the Pokémon challenges in src/ui/index.html), each time they change:
        //   {"challenges": [{"battle": "ab12", "name": "Ash", "color": "#rrggbb", "left": seconds,
        //                    "format": "Gen 9 Random Battle"}...]}
        void show_challenges() {
            std::string list;
            std::string shown;
            const auto now = clock::now();
            for (const Challenge& c : challenges_) {
                if (c.ours || c.accepted) {
                    continue;
                }
                const auto left = std::chrono::duration_cast<std::chrono::seconds>(challenge_time - (now - c.at));
                list += std::format(
                    "{}{{\"battle\":\"{:x}\",\"name\":{},\"color\":\"{}\",\"left\":{},\"format\":{}}}",
                    list.empty() ? "" : ",", c.battle, json(c.name), hex_color(c.from),
                    std::max<long long>(0, left.count()), json(format_name(c.gen)));
                shown += std::format("{:x} ", c.battle);
            }
            if (shown == shown_challenges_) {
                return;
            }
            // Until the window is there to show them (a terminal never is: there they are typed).
            if (terminal_.show_game("pokemon-challenge", std::format("{{\"challenges\":[{}]}}", list))) {
                shown_challenges_ = shown;
            }
        }

        // The challenge to us from who (any, when empty), still waiting for an answer.
        Challenge* incoming(const std::string& who) {
            for (auto it = challenges_.rbegin(); it != challenges_.rend(); ++it) {
                if (!it->ours && !it->accepted && (who.empty() || lowercase(it->name) == lowercase(who))) {
                    return &*it;
                }
            }
            return nullptr;
        }

        void accept(const std::string& who) {
            Challenge* c = incoming(who);
            if (!c) {
                chat_.game_notice(who.empty()
                                      ? "Nobody challenged you (lately)."
                                      : std::format("{} did not challenge you (lately).", text::sanitize(who, 48)));
                return;
            }
            if (own_battle()) {
                chat_.game_notice("Finish your battle first (or /game pokemon forfeit).");
                return;
            }
            const bool node = engine_.has_node();
            if (!node && !c->node) {
                chat_.game_notice(
                    "Neither of you has Node.js, which Pokémon Showdown needs: one of you can install it from "
                    "nodejs.org, then challenge again.");
                send(std::format("decline {:x}", c->battle));
                std::erase_if(challenges_, [&](const Challenge& x) {
                    return &x == c;
                });
                return;
            }
            c->accepted = true;
            c->at = clock::now();
            send(std::format("accept {:x} {}", c->battle, node ? 1 : 0));
            chat_.game_notice(std::format("⚔ You accept {}'s challenge: the battle is starting…", c->colored_name));
        }

        void decline(const std::string& who) {
            Challenge* c = incoming(who);
            if (!c) {
                chat_.game_notice("Nobody challenged you (lately).");
                return;
            }
            send(std::format("decline {:x}", c->battle));
            chat_.game_notice(std::format("You decline {}'s challenge.", c->colored_name));
            std::erase_if(challenges_, [&](const Challenge& x) {
                return &x == c;
            });
        }

        void receive_accept(std::uint64_t sender, std::string_view name, std::uint64_t battle, std::string_view text) {
            const auto it = std::ranges::find_if(challenges_, [&](const Challenge& c) {
                return c.battle == battle && c.ours && !c.accepted && lowercase(c.name) == lowercase(name);
            });
            if (it == challenges_.end()) {
                return;
            }
            const bool their_node = next_field(text) == "1";
            const Challenge c = *it;
            challenges_.erase(it);
            if (own_battle()) {
                send(std::format("cancel {:x} already in another battle", battle));
                return;
            }
            const bool node = engine_.has_node();
            if (!node && !their_node) {
                send(std::format("cancel {:x} neither of you has Node.js", battle));
                chat_.game_notice(
                    "Neither of you has Node.js, which Pokémon Showdown needs: one of you can install it from "
                    "nodejs.org, then challenge again.");
                return;
            }
            // We run the battle when we can.
            const std::uint64_t referee = node ? chat_.id() : sender;
            const std::array<std::uint64_t, 2> ids {chat_.id(), sender};
            const std::array<std::string, 2> names {chat_.name(), std::string(name)};
            send(std::format("start {:x} {:x} {:x} {:x} {} {} {}", battle, referee, ids[0], ids[1],
                             encode_field(names[0]), encode_field(names[1]), format_id(c.gen)));
            begin(battle, referee, ids, names, c.gen);
        }

        void receive_decline(std::uint64_t sender, std::string_view name, std::uint64_t battle) {
            std::erase_if(challenges_, [&](const Challenge& c) {
                if (c.battle == battle && c.ours && lowercase(c.name) == lowercase(name)) {
                    chat_.game_notice(std::format("⚔ {} declined your challenge.", chat_.colored_name(sender, name)));
                    return true;
                }
                return false;
            });
        }

        void receive_start(std::uint64_t sender, std::uint64_t battle, std::string_view text) {
            const auto referee = parse_number(next_field(text), 16);
            const auto p1 = parse_number(next_field(text), 16);
            const auto p2 = parse_number(next_field(text), 16);
            const std::string n1 = text::sanitize(decode_field(next_field(text)), 48);
            const std::string n2 = text::sanitize(decode_field(next_field(text)), 48);
            const int gen = format_gen(next_field(text));
            // Only the challenger starts it.
            if (!referee || !p1 || !p2 || *p1 != sender || battles_.contains(battle)) {
                return;
            }
            std::erase_if(challenges_, [&](const Challenge& c) {
                return c.battle == battle;
            });
            begin(battle, *referee, {*p1, *p2}, {n1, n2}, gen ? gen : default_gen);
        }

        // A battle starts: we may play it, and run it.
        void begin(std::uint64_t id, std::uint64_t referee, std::array<std::uint64_t, 2> ids,
                   std::array<std::string, 2> names, int gen) {
            Battle& b = battles_[id];
            b.id = id;
            b.referee_id = referee;
            b.ids = ids;
            b.names = names;
            b.gen = gen;
            b.last_head = clock::now();
            const std::string vs =
                std::format("{} vs {}", chat_.colored_name(ids[0], names[0]), chat_.colored_name(ids[1], names[1]));
            const int us = ids[0] == chat_.id() ? 0 : ids[1] == chat_.id() ? 1 : -1;
            if (us < 0) {
                chat_.game_notice(std::format("⚔ A Pokémon battle starts ({}): {}! /game pokemon watch {} to watch it.",
                                              format_name(gen), vs, names[0]));
                return;
            }
            b.side = side_name(us);
            b.view.emplace(b.side);
            chat_.game_notice(std::format("⚔ Your Pokémon battle starts ({}): {}! Good luck.", format_name(gen), vs));
            terminal_.bell();
            if (referee == chat_.id()) {
                b.referee = std::make_unique<Referee>();
                const auto now = clock::now();
                b.referee->heard = {now, now};
                b.referee->last_head = now - head_interval;
                if (engine_.status() != pokemon::Engine::Status::Ready) {
                    engine_.install();
                }
            }
            show(b, true);
        }

        // --- Playing

        // The battle we play, while it goes on.
        Battle* own_battle() {
            for (auto& [id, b] : battles_) {
                if (!b.over && !b.side.empty()) {
                    return &b;
                }
            }
            return nullptr;
        }

        void choose(const std::string& typed) {
            Battle* b = own_battle();
            if (!b) {
                chat_.game_notice("You are not in a Pokémon battle: /game pokemon challenge NAME starts one.");
                return;
            }
            std::string error;
            const int rqid = b->view->rqid();
            const auto choice = b->view->parse_choice(typed, error);
            if (!choice) {
                chat_.game_notice(error.empty() ? std::string("You cannot do that now.") : error);
                return;
            }
            b->chose_label = choice_label(*b->view, *choice);
            b->view->chosen();
            b->chose = true;
            b->chose_rqid = rqid;
            do_action(*b, std::format("choose {} {}", rqid, *choice));
            show(*b);
            if (!b->window) {
                chat_.game_notice(
                    std::format("⚔ You chose {}: waiting for the other player (/game pokemon cancel to change "
                                "it).",
                                b->chose_label));
            }
        }

        // What a choice does, in words, from the menu of the request it answers: "Earthquake", "Earthquake, with
        // Mega Evolution", "Gholdengo".
        static std::string choice_label(const pokemon::Battle& view, std::string_view choice) {
            std::string_view rest = choice;
            const std::string_view kind = next_field(rest);
            const std::string_view slot = next_field(rest);
            const std::string prefix = std::format("{} {}  ", kind, slot);
            std::string label = std::format("{} {}", kind, slot);
            for (const auto& line : view.menu()) {
                if (line.starts_with(prefix)) {
                    const std::string_view name = std::string_view(line).substr(prefix.size());
                    label = std::string(name.substr(0, name.find("  ")));
                    break;
                }
            }
            const std::string_view gimmick = rest;
            return gimmick == "terastallize" ? label + ", Terastallizing"
                   : gimmick == "mega"       ? label + ", with Mega Evolution"
                   : gimmick == "ultra"      ? label + ", with Ultra Burst"
                   : gimmick == "zmove"      ? label + ", as a Z-Move"
                   : gimmick == "dynamax"    ? label + ", Dynamaxing"
                                             : label;
        }

        // Takes our choice back, while the other player has not chosen: the turn is played only once both have.
        void cancel() {
            Battle* b = own_battle();
            if (!b || !b->chose || b->view->rqid() != 0) {
                chat_.game_notice("There is no choice to take back now.");
                return;
            }
            if (b->cancelling) {
                return;
            }
            b->cancelling = true;
            b->cancel_n = b->next_action;
            show(*b);
            do_action(*b, std::format("undo {}", b->chose_rqid));
        }

        // The referee's answer to our undo: taken back, or too late.
        void cancelled(Battle& b, std::uint64_t n, std::string_view result) {
            if (!b.cancelling || n != b.cancel_n) {
                return;
            }
            b.cancelling = false;
            if (result == "ok" && b.chose && b.view->rqid() == 0) {
                b.view->unchoose();
                b.chose = false;
                show(b);
                if (!b.window) {
                    chat_.game_notice("⚔ Choice taken back: choose again.");
                }
                return;
            }
            show(b);
            chat_.game_notice("⚔ Too late to take it back: the other player had chosen too.");
        }

        void act(std::string_view action) {
            Battle* b = own_battle();
            if (!b) {
                chat_.game_notice("You are not in a Pokémon battle.");
                return;
            }
            do_action(*b, std::string(action));
        }

        // An action of ours goes to the referee: right away when it is us.
        void do_action(Battle& b, std::string text) {
            const std::uint64_t n = b.next_action++;
            if (b.referee) {
                const std::string result = referee_action(b, side_index(b.side), n, text);
                cancelled(b, n, result);
                return;
            }
            send(std::format("do {:x} {} {}", b.id, n, text));
            b.actions.push_back({n, std::move(text), clock::now()});
        }

        void say(const std::string& text) {
            Battle* b = nullptr;
            for (auto& [id, battle] : battles_) {
                if (battle.view && (!b || !battle.over)) {
                    b = &battle;
                }
            }
            if (!b) {
                chat_.game_notice("You are not in a Pokémon battle, nor watching one.");
                return;
            }
            const std::string clean = text::sanitize(text, max_said_bytes);
            if (clean.empty()) {
                return;
            }
            send(std::format("say {:x} {}", b->id, clean));
            b->view->chat(chat_.name(), hex_color(chat_.id()), clean);
            show(*b);
        }

        void watch(const std::string& who) {
            std::vector<Battle*> found;
            for (auto& [id, b] : battles_) {
                if (!b.over && (who.empty() || lowercase(b.names[0]) == lowercase(who) ||
                                lowercase(b.names[1]) == lowercase(who))) {
                    found.push_back(&b);
                }
            }
            if (found.empty()) {
                chat_.game_notice(who.empty() ? "No Pokémon battle is going on."
                                              : std::format("{} is not in a Pokémon battle.", text::sanitize(who, 48)));
                return;
            }
            if (found.size() > 1) {
                chat_.game_notice("Which one? /game pokemon watch NAME, with the name of one of its players:");
                for (const Battle* b : found) {
                    chat_.game_notice(std::format("   {} vs {}", b->names[0], b->names[1]));
                }
                return;
            }
            Battle& b = *found.front();
            if (b.view) {
                b.window_open = true;
                show(b, true);
                return;
            }
            b.view.emplace(std::string());
            b.side.clear();
            b.in = pokemon::link::Inbound(partial_timeout);
            b.printed = 0;
            b.window_open = true;
            b.last_want = clock::now();
            send(std::format("want {:x} log 1", b.id));
            chat_.game_notice(std::format("👀 Watching {} vs {}: /game pokemon unwatch to stop.",
                                          chat_.colored_name(b.ids[0], b.names[0]),
                                          chat_.colored_name(b.ids[1], b.names[1])));
            show(b, true);
        }

        void unwatch() {
            for (auto& [id, b] : battles_) {
                if (b.view && b.side.empty()) {
                    b.view.reset();
                    b.window_open = false;
                    chat_.game_notice(std::format("You stop watching {} vs {}.", b.names[0], b.names[1]));
                }
            }
        }

        void update() {
            if (!engine_.has_node()) {
                chat_.game_notice("Pokémon Showdown needs Node.js, which is not here: nodejs.org has it.");
                return;
            }
            engine_.install(true);
            chat_.game_notice("📦 Updating Pokémon Showdown with npm (for the newest random battle sets)…");
            updating_ = true;
        }

        // --- What the others say about a battle

        void receive_battle(Battle& b, std::uint64_t sender, std::string_view name, std::string_view event,
                            std::string_view text) {
            if (sender == b.referee_id) {
                b.last_head = clock::now();
            }
            if (b.referee) {
                const int from = sender == b.ids[0] ? 0 : sender == b.ids[1] ? 1 : -1;
                if (from >= 0) {
                    b.referee->heard[from] = clock::now();
                }
                if (event == "ack" && from >= 0) {
                    const auto side = next_field(text);
                    const auto seq = parse_number(next_field(text));
                    if (side_index(side) == from && seq) {
                        Outbound& out = b.referee->streams[1 + from];
                        out.acked = std::max(out.acked, std::min<std::uint64_t>(*seq, out.messages.size()));
                    }
                } else if (event == "want") {
                    receive_want(b, from, text);
                } else if (event == "do" && from >= 0) {
                    if (const auto n = parse_number(next_field(text))) {
                        referee_action(b, from, *n, std::string(text));
                    }
                }
            }
            if (event == "head" && sender == b.referee_id) {
                receive_head(sender, b.id, text);
            } else if (event == "end" && sender == b.referee_id) {
                const std::string_view outcome = next_field(text);
                finish(b, std::string(outcome), text::sanitize(text, 200));
            } else if ((event == "log" || event == "priv") && sender == b.referee_id && b.view && !b.referee) {
                receive_stream(b, event, text);
            } else if (event == "done" && sender == b.referee_id) {
                const auto side = next_field(text);
                const auto n = parse_number(next_field(text));
                if (side == b.side && n) {
                    std::erase_if(b.actions, [&](const Action& a) {
                        return a.n == *n;
                    });
                    cancelled(b, *n, next_field(text));
                }
            } else if (event == "say" && b.view) {
                const std::string clean = text::sanitize(text, max_said_bytes);
                if (!clean.empty()) {
                    b.view->chat(name, hex_color(sender), clean);
                    show(b);
                }
            }
        }

        void receive_stream(Battle& b, std::string_view event, std::string_view text) {
            if (event == "priv") {
                if (b.side.empty() || next_field(text) != b.side) {
                    return;
                }
            } else if (!b.side.empty()) {
                // Players have their own stream, which has everything.
                return;
            }
            const auto seq = parse_number(next_field(text));
            const std::string_view parts = next_field(text);
            const auto slash = parts.find('/');
            if (!seq || slash == std::string_view::npos) {
                return;
            }
            const auto part = parse_number(parts.substr(0, slash));
            const auto count = parse_number(parts.substr(slash + 1));
            if (!part || !count || *part == 0) {
                return;
            }
            const auto messages = b.in.add(*seq, *part - 1, *count, text);
            for (const auto& message : messages) {
                b.view->feed(unescape(message));
            }
            if (!messages.empty()) {
                b.ack_due = !b.side.empty();
                show(b);
            }
        }

        void receive_head(std::uint64_t sender, std::uint64_t id, std::string_view text) {
            std::array<std::uint64_t, 6> numbers {};
            for (auto& n : numbers) {
                const std::string_view field = next_field(text);
                if (field == "-1") {
                    n = static_cast<std::uint64_t>(-1);
                } else if (const auto value = parse_number(field)) {
                    n = *value;
                } else {
                    return;
                }
            }
            const std::string outcome(next_field(text));
            const auto p1 = parse_number(next_field(text), 16);
            const auto p2 = parse_number(next_field(text), 16);
            const std::string n1 = text::sanitize(decode_field(next_field(text)), 48);
            const std::string n2 = text::sanitize(decode_field(next_field(text)), 48);
            const int gen = format_gen(next_field(text));
            if (!p1 || !p2) {
                return;
            }
            auto it = battles_.find(id);
            if (it == battles_.end()) {
                // One that started before we came: we can watch it too.
                if (outcome != "0") {
                    return;
                }
                Battle& b = battles_[id];
                b.id = id;
                b.referee_id = sender;
                b.ids = {*p1, *p2};
                b.names = {n1, n2};
                b.gen = gen ? gen : default_gen;
                it = battles_.find(id);
            }
            Battle& b = it->second;
            if (sender != b.referee_id) {
                return;
            }
            b.last_head = clock::now();
            if (b.view && !b.referee) {
                const int us = side_index(b.side);
                b.head_count = us < 0 ? numbers[0] : numbers[1 + us];
                const bool timer = numbers[3] != 0;
                const int left = us < 0 || numbers[4 + us] == static_cast<std::uint64_t>(-1)
                                     ? -1
                                     : static_cast<int>(numbers[4 + us]);
                if (timer != b.timer || left != b.left) {
                    b.timer = timer;
                    b.left = left;
                    b.left_at = clock::now();
                    show(b);
                }
            }
            if (outcome != "0") {
                finish(b, outcome, "");
            }
        }

        // --- Watching and playing: asking for what is missing, saying we are here, showing it

        void viewer_tick(Battle& b) {
            const auto now = clock::now();
            if (!b.referee) {
                b.in.drop_stale();
                // Missing messages: asked for again.
                if (b.in.received() < b.head_count && now - b.last_want >= want_interval) {
                    b.last_want = now;
                    send(std::format("want {:x} {} {}", b.id, b.side.empty() ? "log" : b.side, b.in.received() + 1));
                }
                if (!b.side.empty() && (b.ack_due || now - b.last_ack >= ack_interval) && !b.over) {
                    b.ack_due = false;
                    b.last_ack = now;
                    send(std::format("ack {:x} {} {}", b.id, b.side, b.in.received()));
                }
                for (Action& a : b.actions) {
                    if (now - a.sent >= resend_interval) {
                        a.sent = now;
                        send(std::format("do {:x} {} {}", b.id, a.n, a.text));
                    }
                }
                if (!b.over && now - b.last_head > referee_silence) {
                    finish(b, "none",
                           std::format("{} (who ran it) left", b.referee_id == b.ids[0] ? b.names[0] : b.names[1]));
                }
            }
            // The seconds left, as they go.
            if (b.timer && b.left >= 0) {
                const int left = seconds_left(b);
                if (left != b.last_left_shown) {
                    b.last_left_shown = left;
                    show(b);
                }
            }
        }

        int seconds_left(const Battle& b) const {
            if (!b.timer || b.left < 0) {
                return -1;
            }
            const auto gone = std::chrono::duration_cast<std::chrono::seconds>(clock::now() - b.left_at).count();
            return std::max(0, b.left - static_cast<int>(gone));
        }

        // Shows the battle: in its window, or else printed (what is new since last time). open opens the window
        // again if it was closed.
        void show(Battle& b, bool open = false) {
            if (!b.view) {
                return;
            }
            if (open) {
                b.window_open = true;
            }
            const pokemon::Battle& v = *b.view;
            if (v.rqid() != 0) {
                b.chose = false;
            }
            const bool setup = !b.over && v.log().empty();
            std::string setup_text = "Starting the battle…";
            if (b.referee && !b.referee->running) {
                setup_text = engine_.status() == pokemon::Engine::Status::Installing
                                 ? "Installing Pokémon Showdown, the first time (about a minute)…"
                                 : "Starting Pokémon Showdown…";
            }
            std::string result;
            if (b.over) {
                const int winner = side_index(b.outcome);
                result = winner >= 0          ? std::format("{} won!", b.names[winner])
                         : b.outcome == "tie" ? std::string("It's a tie!")
                         : b.reason.empty()   ? std::string("The battle ended.")
                                              : std::format("The battle ended: {}.", b.reason);
            }
            const std::string extra = std::format(
                "\"battle\":\"{:x}\",\"format\":{},\"title\":{},\"phase\":\"{}\",\"setup\":{},\"colors\":{{\"p1\":\"{}\",\"p2\":\"{}"
                "\"}},"
                "\"timer\":{{\"on\":{},\"left\":{}}},\"chosen\":{},\"choice\":{},\"cancelling\":{},\"watching\":{},\"result\":{},"
                "\"open\":{}",
                b.id, json(format_name(b.gen)), json(std::format("{} vs {}", b.names[0], b.names[1])),
                b.over  ? "over"
                : setup ? "setup"
                        : "battle",
                json(setup_text), hex_color(b.ids[0]), hex_color(b.ids[1]), b.timer,
                seconds_left(b) < 0 ? std::string("null") : std::to_string(seconds_left(b)),
                b.chose && v.rqid() == 0 && !b.over, json(b.chose && v.rqid() == 0 ? b.chose_label : std::string()),
                b.cancelling,
                b.side.empty(), json(result), b.window_open);
            b.window = terminal_.show_game(game_name, v.json(extra));
            if (b.window) {
                b.printed = v.log().size();
                return;
            }
            print_new(b);
        }

        // In the terminal: the new lines of the log, then the choices when it is our turn.
        void print_new(Battle& b) {
            const pokemon::Battle& v = *b.view;
            const auto& log = v.log();
            for (; b.printed < log.size(); ++b.printed) {
                const pokemon::LogLine& line = log[b.printed];
                using Kind = pokemon::LogLine::Kind;
                switch (line.kind) {
                case Kind::Turn:
                    chat_.game_notice(std::format("⚔ ── {} ──", line.text));
                    break;
                case Kind::Chat:
                    chat_.game_notice(std::format("⚔ {}: {}", line.name, line.text));
                    break;
                case Kind::Minor:
                    chat_.game_notice(std::format("⚔   {}", line.text));
                    break;
                default:
                    chat_.game_notice(std::format("⚔ {}", line.text));
                    break;
                }
            }
            if (v.rqid() != 0 && v.rqid() != b.menu_rqid) {
                b.menu_rqid = v.rqid();
                chat_.game_notice("⚔ Your move: /game pokemon then one of");
                for (const auto& choice : v.menu()) {
                    chat_.game_notice(std::format("⚔   {}", choice));
                }
            }
        }

        // --- Running a battle, as its referee

        void referee_tick(Battle& b) {
            Referee& r = *b.referee;
            const auto now = clock::now();
            if (!r.running && !b.over) {
                switch (engine_.status()) {
                case pokemon::Engine::Status::Ready:
                    run(b);
                    break;
                case pokemon::Engine::Status::Installing:
                    if (!r.announced_install) {
                        r.announced_install = true;
                        chat_.game_notice("📦 Installing Pokémon Showdown with npm, the first time (about a minute)…");
                        show(b);
                    }
                    break;
                case pokemon::Engine::Status::Failed:
                case pokemon::Engine::Status::Missing:
                    chat_.game_notice(std::format("Pokémon Showdown could not be installed: {}", engine_.failure()));
                    end(b, "none", "Pokémon Showdown could not be installed");
                    return;
                }
            }
            drain(b);
            if (!b.referee) {
                return;
            }
            // A player's messages not acknowledged: again.
            for (int side = 0; side < 2; ++side) {
                if (side_index(b.side) == side) {
                    continue;
                }
                Outbound& out = r.streams[1 + side];
                if (out.acked < out.messages.size() && now - out.resent >= resend_interval) {
                    out.resent = now;
                    for (std::uint64_t seq = out.acked + 1; seq <= out.messages.size() && seq <= out.acked + max_resend;
                         ++seq) {
                        send_message(b, 1 + side, seq);
                    }
                }
            }
            if (!b.over) {
                referee_timer(b);
                for (int side = 0; side < 2; ++side) {
                    if (side_index(b.side) != side && now - r.heard[side] > player_silence) {
                        inject(b, std::format("|message|{} left.", battle_name(b.names[side])));
                        if (r.child) {
                            r.child->write(std::format("forcewin {}", side_name(1 - side)));
                        }
                        r.heard[side] = now;
                    }
                }
            }
            if (now - r.last_head >= (r.timer ? head_interval_timer : head_interval)) {
                send_head(b);
            }
            if (b.over && now - b.ended > keep_referee) {
                b.referee.reset();
            }
        }

        void run(Battle& b) {
            Referee& r = *b.referee;
            std::string error;
            std::weak_ptr<Inbox> inbox = r.inbox;
            r.child = engine_.start_battle(
                b.gen, battle_name(b.names[0]), battle_name(b.names[1]),
                [inbox](std::string line) {
                    if (const auto in = inbox.lock()) {
                        std::scoped_lock lock(in->mutex);
                        in->lines.push_back(std::move(line));
                    }
                },
                [inbox](int code, std::string errors) {
                    if (const auto in = inbox.lock()) {
                        std::scoped_lock lock(in->mutex);
                        in->exit = {code, std::move(errors)};
                    }
                },
                error);
            if (!r.child) {
                chat_.game_notice(std::format("Pokémon Showdown could not be started: {}", error));
                end(b, "none", "Pokémon Showdown could not be started");
                return;
            }
            r.running = true;
        }

        // What the bridge said since last time.
        void drain(Battle& b) {
            Referee& r = *b.referee;
            std::deque<std::string> lines;
            std::optional<std::pair<int, std::string>> exit;
            {
                std::scoped_lock lock(r.inbox->mutex);
                lines.swap(r.inbox->lines);
                exit.swap(r.inbox->exit);
            }
            for (const auto& line : lines) {
                const auto space = line.find(' ');
                const std::string_view tag = std::string_view(line).substr(0, space);
                const std::string_view text =
                    space == std::string::npos ? std::string_view() : std::string_view(line).substr(space + 1);
                if (tag == "spectator") {
                    add_message(b, 0, std::string(text));
                    // How it ends: "|win|NAME" or "|tie", lines of their own ("|tier|" is something else).
                    const std::string plain = unescape(text);
                    std::string_view rest = plain;
                    while (!rest.empty() && !b.over) {
                        const auto nl = rest.find('\n');
                        const std::string_view protocol = rest.substr(0, nl);
                        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
                        if (protocol.starts_with("|win|")) {
                            const std::string winner(protocol.substr(5));
                            const int side = winner == battle_name(b.names[0])   ? 0
                                             : winner == battle_name(b.names[1]) ? 1
                                                                                 : -1;
                            end(b, side < 0 ? "none" : std::string(side_name(side)),
                                side < 0 ? "who won is unclear" : "");
                        } else if (protocol == "|tie" || protocol.starts_with("|tie|")) {
                            end(b, "tie", "");
                        }
                    }
                } else if (tag == "p1" || tag == "p2") {
                    const int side = side_index(tag);
                    note_request(b, side, unescape(text));
                    add_message(b, 1 + side, std::string(text));
                } else if (tag == "error") {
                    chat_.game_notice(std::format("Pokémon Showdown: {}", text::sanitize(unescape(text), 400)));
                }
                if (!b.referee) {
                    return;
                }
            }
            if (exit && !b.over) {
                if (!exit->second.empty()) {
                    chat_.game_notice(std::format("Pokémon Showdown stopped: {}", text::sanitize(exit->second, 400)));
                }
                end(b, "none", "the battle engine stopped");
            }
        }

        // Keeps what a player is asked, for the timer.
        void note_request(Battle& b, int side, const std::string& message) {
            Referee& r = *b.referee;
            if (message.find("|error|") != std::string::npos && r.rqid[side] != 0) {
                // A choice refused: to be made again. But an undo refused (it would tell something of the other
                // side) leaves the choice made.
                r.waiting[side] = message.find("Can't undo") == std::string::npos;
                return;
            }
            const auto at = message.find("|request|");
            if (at == std::string::npos) {
                return;
            }
            const auto key = message.find("\"rqid\":", at);
            int rqid = 0;
            if (key != std::string::npos) {
                std::size_t end = key + 7;
                while (end < message.size() && message[end] >= '0' && message[end] <= '9') {
                    ++end;
                }
                rqid = parse_int(std::string_view(message).substr(key + 7, end - key - 7)).value_or(0);
            }
            const bool wait = message.find("\"wait\":true", at) != std::string::npos;
            if (rqid != r.rqid[side]) {
                r.rqid[side] = rqid;
                r.waiting[side] = !wait;
                r.asked[side] = clock::now();
                r.warned[side].clear();
            }
        }

        // A message of a stream (0: log, 1: p1, 2: p2): kept, and sent; ours goes to our own battle.
        void add_message(Battle& b, int stream, std::string text) {
            Outbound& out = b.referee->streams[stream];
            out.messages.push_back(std::move(text));
            const std::uint64_t seq = out.messages.size();
            if (stream > 0 && side_index(b.side) == stream - 1) {
                out.acked = seq;
                if (b.view) {
                    b.view->feed(unescape(out.messages.back()));
                    show(b);
                }
                return;
            }
            send_message(b, stream, seq);
        }

        void send_message(Battle& b, int stream, std::uint64_t seq) {
            const std::string& message = b.referee->streams[stream].messages[seq - 1];
            const auto parts = pokemon::link::split(message, part_bytes);
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (stream == 0) {
                    send(std::format("log {:x} {} {}/{} {}", b.id, seq, i + 1, parts.size(), parts[i]));
                } else {
                    send(std::format("priv {:x} {} {} {}/{} {}", b.id, side_name(stream - 1), seq, i + 1, parts.size(),
                                     parts[i]));
                }
            }
        }

        // A line for everyone in the battle, as if the simulator had said it.
        void inject(Battle& b, const std::string& line) {
            std::string escaped;
            for (const char c : line) {
                escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
            }
            for (int stream = 0; stream < 3; ++stream) {
                add_message(b, stream, escaped);
            }
        }

        void receive_want(Battle& b, int from, std::string_view text) {
            Referee& r = *b.referee;
            const std::string_view stream_name = next_field(text);
            const auto first = parse_number(next_field(text));
            const int stream = stream_name == "log" ? 0 : stream_name == "p1" ? 1 : stream_name == "p2" ? 2 : -1;
            // A player's stream only to that player.
            if (stream < 0 || !first || *first == 0 || (stream > 0 && from != stream - 1)) {
                return;
            }
            const auto now = clock::now();
            auto& last = r.wants[std::format("{} {}", stream, *first)];
            if (now - last < want_interval) {
                return;
            }
            last = now;
            const auto& messages = r.streams[stream].messages;
            for (std::uint64_t seq = *first; seq <= messages.size() && seq < *first + max_resend; ++seq) {
                send_message(b, stream, seq);
            }
        }

        // A player's action, which comes once, though it may be sent many times.
        // A player's N-th action, which comes once, though it may be sent many times, and in the order they did them:
        // a choice made again after taking one back must not come before the undo, which would undo it.
        // Returns what our own action did (see referee_do()).
        std::string referee_action(Battle& b, int side, std::uint64_t n, const std::string& action) {
            Referee& r = *b.referee;
            if (side_index(b.side) == side) {
                // Ours, which come right away, in order.
                return referee_do(b, side, action);
            }
            if (n < r.next_action[side]) {
                // Done already: they did not hear it.
                const auto it = r.results[side].find(n);
                send(std::format("done {:x} {} {} {}", b.id, side_name(side), n,
                                 it == r.results[side].end() ? std::string() : it->second));
                return {};
            }
            // Those that come too early wait for the ones before them (they are sent again until done).
            if (n >= r.next_action[side] + 64) {
                return {};
            }
            r.early_actions[side].emplace(n, action);
            for (auto it = r.early_actions[side].find(r.next_action[side]); it != r.early_actions[side].end();
                 it = r.early_actions[side].find(r.next_action[side])) {
                const std::uint64_t done = it->first;
                const std::string next = std::move(it->second);
                r.early_actions[side].erase(it);
                ++r.next_action[side];
                const std::string result = referee_do(b, side, next);
                if (!b.referee) {
                    return {};
                }
                r.results[side][done] = result;
                if (r.results[side].size() > 64) {
                    r.results[side].erase(r.results[side].begin());
                }
                send(std::format("done {:x} {} {} {}", b.id, side_name(side), done, result));
            }
            return {};
        }

        // Does a player's action; for an undo, returns ok, or late when it could not be (else empty).
        std::string referee_do(Battle& b, int side, const std::string& action) {
            Referee& r = *b.referee;
            if (b.over || !r.child) {
                return action.starts_with("undo") ? "late" : "";
            }
            std::string_view rest = action;
            const std::string_view verb = next_field(rest);
            const std::string player = battle_name(b.names[side]);
            if (verb == "choose") {
                const auto rqid = parse_int(next_field(rest));
                // An old choice, for a request answered already: choosing again takes an undo first (or else, the
                // other player having chosen, it would go to the next turn).
                if (!rqid || *rqid != r.rqid[side] || !r.waiting[side] || rest.empty()) {
                    return {};
                }
                r.waiting[side] = false;
                r.child->write(std::format("{} {}", side_name(side), rest));
            } else if (verb == "undo") {
                // Only while the other player is still choosing: once they have (or have nothing to choose), the
                // simulator plays the turn with the choice, even if its next request has not come out yet.
                const auto rqid = parse_int(next_field(rest));
                if (!rqid || *rqid != r.rqid[side] || r.waiting[side] || !r.waiting[1 - side]) {
                    return "late";
                }
                r.waiting[side] = true;
                r.child->write(std::format("{} undo", side_name(side)));
                return "ok";
            } else if (verb == "forfeit") {
                inject(b, std::format("|message|{} forfeited.", player));
                r.child->write(std::format("forcewin {}", side_name(1 - side)));
            } else if (verb == "timer") {
                r.timer = !r.timer;
                const auto now = clock::now();
                for (int s = 0; s < 2; ++s) {
                    r.asked[s] = now;
                    r.warned[s].clear();
                }
                inject(b, r.timer ? std::format("|inactive|Battle timer is ON: inactive players will automatically "
                                                "lose when time's up. (requested by {})",
                                                player)
                                  : std::format("|inactiveoff|Battle timer is now OFF. (turned off by {})", player));
                send_head(b);
            }
            return {};
        }

        int referee_left(const Battle& b, int side) const {
            const Referee& r = *b.referee;
            if (!r.timer || !r.waiting[side]) {
                return -1;
            }
            const auto gone = std::chrono::duration_cast<std::chrono::seconds>(clock::now() - r.asked[side]).count();
            return std::max(0, turn_seconds - static_cast<int>(gone));
        }

        void referee_timer(Battle& b) {
            Referee& r = *b.referee;
            if (!r.timer) {
                return;
            }
            for (int side = 0; side < 2; ++side) {
                const int left = referee_left(b, side);
                if (left < 0) {
                    continue;
                }
                const std::string player = battle_name(b.names[side]);
                for (const int warning : warnings) {
                    if (left <= warning && r.warned[side].insert(warning).second) {
                        inject(b, std::format("|inactive|{} has {} seconds left.", player, warning));
                    }
                }
                if (left == 0) {
                    r.waiting[side] = false;
                    inject(b, std::format("|message|{} ran out of time.", player));
                    if (r.child) {
                        r.child->write(std::format("forcewin {}", side_name(1 - side)));
                    }
                    return;
                }
            }
        }

        void send_head(Battle& b) {
            Referee& r = *b.referee;
            r.last_head = clock::now();
            const auto left = [&](int side) {
                const int l = referee_left(b, side);
                return l < 0 ? std::string("-1") : std::to_string(l);
            };
            send(std::format("head {:x} {} {} {} {} {} {} {} {:x} {:x} {} {} {}", b.id, r.streams[0].messages.size(),
                             r.streams[1].messages.size(), r.streams[2].messages.size(), r.timer ? 1 : 0, left(0),
                             left(1), b.over ? b.outcome : "0", b.ids[0], b.ids[1], encode_field(b.names[0]),
                             encode_field(b.names[1]), format_id(b.gen)));
            // Our own side sees the timer too.
            if (b.view) {
                const int us = side_index(b.side);
                const int l = us < 0 ? -1 : referee_left(b, us);
                if (r.timer != b.timer || l != b.left) {
                    b.timer = r.timer;
                    b.left = l;
                    b.left_at = clock::now();
                    show(b);
                }
            }
        }

        // The referee ends the battle, and tells everybody.
        void end(Battle& b, const std::string& outcome, const std::string& reason) {
            if (b.over) {
                return;
            }
            send(std::format("end {:x} {} {}", b.id, outcome, reason));
            finish(b, outcome, reason);
            if (b.referee) {
                send_head(b);
            }
        }

        // --- The end

        void finish(Battle& b, const std::string& outcome, const std::string& reason) {
            if (b.over) {
                return;
            }
            b.over = true;
            b.outcome = outcome;
            b.reason = reason;
            b.ended = clock::now();
            b.actions.clear();
            const std::string p1 = chat_.colored_name(b.ids[0], b.names[0]);
            const std::string p2 = chat_.colored_name(b.ids[1], b.names[1]);
            const int winner = side_index(outcome);
            if (winner >= 0) {
                const int wins = games_.add_win(game_name, b.ids[winner], b.names[winner]);
                chat_.game_notice(std::format("🏆 {} won the Pokémon battle against {}!{}", winner == 0 ? p1 : p2,
                                              winner == 0 ? p2 : p1,
                                              wins > 1 ? std::format(" ({} wins)", wins) : std::string()));
                games_.rate(game_name,
                            {{b.ids[winner], b.names[winner], 0}, {b.ids[1 - winner], b.names[1 - winner], 1}});
            } else if (outcome == "tie") {
                chat_.game_notice(std::format("⚔ The Pokémon battle between {} and {} is a tie!", p1, p2));
                games_.rate(game_name, {{b.ids[0], b.names[0], 0}, {b.ids[1], b.names[1], 0}});
            } else {
                chat_.game_notice(std::format("⚔ The Pokémon battle between {} and {} ended{}.", p1, p2,
                                              reason.empty() ? std::string() : std::format(": {}", reason)));
            }
            if (b.view) {
                if (winner < 0 && outcome != "tie" && !reason.empty()) {
                    b.view->notice(std::format("The battle ended: {}.", reason));
                }
                show(b);
            }
        }

        void send(std::string_view event) {
            chat_.send_game(std::format("{} {}", game_name, event));
        }

        Chat& chat_;
        Screen& terminal_;
        Games& games_;
        std::mt19937_64 rng_;
        pokemon::Engine engine_;
        bool updating_ = false;
        std::vector<Challenge> challenges_;
        // The challenges last shown in the window, see show_challenges().
        std::string shown_challenges_;
        std::map<std::uint64_t, Battle> battles_;
    };

} // namespace

std::unique_ptr<Game> make_pokemon(Chat& chat, Screen& terminal, Games& games) {
    return std::make_unique<Pokemon>(chat, terminal, games);
}

} // namespace zchat::game
