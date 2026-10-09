// The world: a room everybody in the chat can walk around in together, like Habbo Hotel, in a window of its own
// (/world, or 🌍 Enter World at the top of the window). Each one is a crewmate of their name's color with their
// avatar on the visor, seen from above; what they say in #general shows over their head, and squares on the floor
// open what the buttons at the top of the window open (the casino, a Pokémon challenge, the other games). A terminal
// cannot show it: /world there tells who is in it.
//
// There is no referee: each one walks in their own window, and tells the others where they are. As Game packets:
//   world at <room> <x> <y> <left> <walking>   where we are (see world::Place): when it changes (the window says it
//                                              at most 12 times a second), and again every second while we stay
//   world leave                                we left the world (closed its window)
// Whoever is not heard from for a while, or left the chat, is not in the world any more. A newcomer hears from the
// others right away: whoever is in the world answers the first place they hear from somebody with their own.
//
// The window talks to zchat with /world at ... (where we are, as in the packets; the first one enters the world),
// /world leave and /world say TEXT (said in #general, whatever channel the chat shows: see Games::world()), and gets
// the state of the world as zchat.game("world", ...).

#include "game.hpp"

#include "color.hpp"
#include "world_place.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;

    constexpr std::string_view game_name = "world";

    // Where we are, told again so often while we stay; and whoever is silent for so long is gone.
    constexpr auto heartbeat = 1s;
    constexpr auto timeout = 5s;
    // Answering a newcomer, at most so often.
    constexpr auto answer_interval = 200ms;
    // Who left the chat is looked for so often.
    constexpr auto prune_interval = 500ms;
    // The window gets the state at most so often while we are in the world.
    constexpr auto publish_interval = 40ms;
    // A bubble over a head is shown for a while (the window decides how long): after this, it is not sent any more.
    constexpr auto bubble_life = 30s;

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

    class World : public Game {
    public:
        World(Chat& chat, Screen& terminal) :
            chat_(chat),
            terminal_(terminal) {
        }

        ~World() override {
            if (in_) {
                chat_.set_fast_ticks(false, fast_ticks_bit);
            }
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "a room to walk around in with everybody, talking over your heads";
        }

        bool listed() const override {
            return false;
        }

        // /world: the window opens it, or shows it again; a terminal tells who is in it.
        void start() override {
            if (publish(true)) {
                return;
            }
            chat_.notice("🌍 The world: a room to walk around in with everybody, where what each one says shows over "
                         "their head. It is in the window: zchat without --terminal, then 🌍 Enter World at the top.");
            tell_who();
        }

        bool command(std::string_view args) override {
            const auto space = args.find(' ');
            std::string verb(args.substr(0, space));
            std::ranges::transform(verb, verb.begin(), [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            });
            const std::string_view rest = space == std::string_view::npos ? std::string_view() : args.substr(space + 1);
            if (verb == "at") {
                const auto place = world::parse_place(rest);
                if (!place) {
                    chat_.notice("/world at ROOM X Y LEFT WALKING: where you are, as the window tells it.");
                    return true;
                }
                if (!in_) {
                    in_ = true;
                    said_ = {};
                    chat_.set_fast_ticks(true, fast_ticks_bit);
                    roster_changed_ = true;
                }
                place_ = *place;
                send_place();
                dirty_ = true;
                return true;
            }
            if (verb == "leave" && rest.empty()) {
                if (in_) {
                    in_ = false;
                    chat_.set_fast_ticks(false, fast_ticks_bit);
                    send("leave");
                    roster_changed_ = dirty_ = true;
                }
                return true;
            }
            if (verb == "who" && rest.empty()) {
                tell_who();
                return true;
            }
            return false;
        }

        void receive(std::uint64_t sender, std::string_view name, std::string_view text) override {
            const auto space = text.find(' ');
            const std::string_view verb = text.substr(0, space);
            if (verb == "leave") {
                if (players_.erase(sender) > 0) {
                    roster_changed_ = dirty_ = true;
                }
                return;
            }
            if (verb != "at" || space == std::string_view::npos) {
                return;
            }
            const auto place = world::parse_place(text.substr(space + 1));
            if (!place) {
                return;
            }
            const auto [it, fresh] = players_.try_emplace(sender);
            Player& p = it->second;
            if (fresh || p.place.room != place->room) {
                roster_changed_ = true;
                // A newcomer (to our room) hears where we are right away, not in a second.
                answer_ = in_ && place->room == place_.room;
            }
            p.name = std::string(name);
            p.place = *place;
            p.seen = clock::now();
            dirty_ = true;
        }

        // Every chat line of #general, ours included: over the head of whoever said it, when they are in the world.
        void message(std::uint64_t sender, std::string_view name, std::string_view text) override {
            (void)name;
            Said* said = nullptr;
            if (sender == chat_.id()) {
                said = in_ ? &said_ : nullptr;
            } else if (const auto it = players_.find(sender); it != players_.end()) {
                said = &it->second.said;
            }
            if (!said) {
                return;
            }
            std::string bubble = world::bubble_text(text);
            if (bubble.empty()) {
                return;
            }
            said->text = std::move(bubble);
            ++said->count;
            said->at = clock::now();
            dirty_ = true;
        }

        void tick() override {
            const auto now = clock::now();
            if (now >= next_prune_) {
                next_prune_ = now + prune_interval;
                std::set<std::uint64_t> online;
                for (const auto& [id, name] : chat_.people()) {
                    online.insert(id);
                }
                const auto gone = std::erase_if(players_, [&](const auto& entry) {
                    return now - entry.second.seen > timeout || !online.contains(entry.first);
                });
                if (gone > 0) {
                    roster_changed_ = dirty_ = true;
                }
            }
            if (in_ && (now - sent_ >= heartbeat || (answer_ && now - sent_ >= answer_interval))) {
                send_place();
            }
            // Out of the world, the window only needs to know how many are in it (for its button).
            if (dirty_ && (in_ ? now - published_ >= publish_interval : roster_changed_)) {
                publish(false);
            }
        }

    private:
        // The world shares the chat's fast ticks with the real-time games: each holds its own.
        static constexpr unsigned fast_ticks_bit = 8;

        // The last thing somebody said, over their head.
        struct Said {
            std::string text;
            unsigned count = 0;
            clock::time_point at;
        };

        struct Player {
            std::string name;
            world::Place place;
            clock::time_point seen;
            Said said;
        };

        void send(std::string_view event) {
            chat_.send_game(std::format("{} {}", game_name, event));
        }

        void send_place() {
            send("at " + world::encode_place(place_));
            sent_ = clock::now();
            answer_ = false;
        }

        // The names of who is in the world, by room, us first.
        void tell_who() const {
            std::map<std::string, std::vector<std::string>> rooms;
            if (in_) {
                rooms[place_.room].push_back(chat_.colored_own_name() + " (you)");
            }
            for (const auto& [id, p] : players_) {
                rooms[p.place.room].push_back(chat_.colored_name(id, p.name));
            }
            if (rooms.empty()) {
                chat_.notice("🌍 Nobody is in the world now.");
                return;
            }
            for (const auto& [room, names] : rooms) {
                std::string list;
                for (const auto& name : names) {
                    list += (list.empty() ? "" : ", ") + name;
                }
                chat_.notice(std::format("🌍 In the {}: {}", room, list));
            }
        }

        // Tells the window (see Screen::show_game()); with open, it opens the world's window too. Returns whether
        // it could: a terminal cannot.
        bool publish(bool open) {
            const auto now = clock::now();
            const std::string room = in_ ? place_.room : std::string(world::lobby);
            std::string players = "[";
            const auto add = [&](std::uint64_t id, std::string_view name, const world::Place& at, const Said& said,
                                 bool you) {
                const Color c = color_of_id(id);
                const bool fresh = said.count > 0 && now - said.at < bubble_life;
                const auto ago = std::chrono::duration_cast<std::chrono::milliseconds>(now - said.at).count();
                players += std::format(
                    "{}{{\"id\":\"{:x}\",\"name\":{},\"color\":\"#{:02x}{:02x}{:02x}\",\"avatar\":{},\"you\":{},"
                    "\"x\":{},\"y\":{},\"left\":{},\"walking\":{},\"say\":{},\"said\":{},\"ago\":{}}}",
                    players.size() > 1 ? "," : "", id, json(name), c.r, c.g, c.b, json(chat_.avatar_of(id)), you, at.x,
                    at.y, at.left, at.walking, json(fresh ? said.text : std::string()), said.count, fresh ? ago : 0);
            };
            if (in_) {
                add(chat_.id(), chat_.name(), place_, said_, true);
            }
            std::size_t count = in_ ? 1 : 0;
            for (const auto& [id, p] : players_) {
                ++count;
                if (p.place.room == room) {
                    add(id, p.name, p.place, p.said, false);
                }
            }
            players += "]";
            const std::string state = std::format("{{\"open\":{},\"in\":{},\"room\":{},\"count\":{},\"players\":{}}}",
                                                  open, in_, json(room), count, players);
            published_ = now;
            dirty_ = roster_changed_ = false;
            return terminal_.show_game(game_name, state);
        }

        Chat& chat_;
        Screen& terminal_;

        // Whether we are in the world (its window is open), where, and what we said last.
        bool in_ = false;
        world::Place place_;
        Said said_;
        clock::time_point sent_;
        // Somebody new came to our room: they hear where we are soon.
        bool answer_ = false;

        // The others in the world, by sender id.
        std::map<std::uint64_t, Player> players_;
        clock::time_point next_prune_;

        // Something changed since the window was told; somebody came or went.
        bool dirty_ = false;
        bool roster_changed_ = false;
        clock::time_point published_;
    };

} // namespace

std::unique_ptr<Game> make_world(Chat& chat, Screen& terminal, Games& games) {
    (void)games;
    return std::make_unique<World>(chat, terminal);
}

} // namespace zchat::game
