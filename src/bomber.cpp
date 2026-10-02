// Bomber: Bomberman for the whole chat, top down on a grid of walls and crates. Everyone who joins gets a corner (or
// a side) of the map; bombs blow up in a cross after a while, breaking crates, which may hide power-ups (more bombs,
// longer flames, faster legs), and blowing up whoever is in the flames, and other bombs. The last one standing wins;
// after two minutes the walls close in, so every round ends. Up to 16 can play: the map grows with them. In a window
// it has a window of its own (see Screen::show_game()), played with the arrows (or WASD) and Space; in a terminal,
// with /game bomber up 3, /game bomber bomb and /game bomber map.
//
// It is played in real time, so unlike the other games it has a referee all along: whoever starts the round runs it,
// 20 steps a second, and sends everybody what the map looks like after each step. The players only send their keys.
// As Game packets, the referee sends:
//   bomber open <round> <seconds> <id>...           the round can be joined for so many seconds more, by these so far
//   bomber off <round>                              the round is off (not enough players)
//   bomber roster <round> <width> <height> <id>...  the round is on, with these players (by sender id, in hex), every
//                                                   second: who comes in the middle can watch
//   bomber s <round> <step> <phase> <seconds> <map> <player>...
//                                                   after each step: the phase (c: counting down, p: playing,
//                                                   h: hurry, the walls close in), the seconds left (to go, or to the
//                                                   hurry), the map (a character per tile, row by row: # wall, + crate,
//                                                   . floor, o bomb, * flame, b f s power-ups) and the players, in the
//                                                   order of the roster, as x,y,alive,bombs,fire,speed,killer (x and
//                                                   y in 24ths of a tile; killer: who blew them up, by place in the
//                                                   roster, -1 for nobody, -2 for the walls)
//   bomber end <round> <place>...                   it is over: where each player finished, 0 for the first (a few
//                                                   times, as everybody rates it)
// and the players send:
//   bomber join <round>                             to play, again every second until they are in the open list
//   bomber in <round> <key> <bombs> [<tile>]        the key they hold (u d l r, or n for none), how many times they
//                                                   asked for a bomb this round, and where to stop (a terminal's
//                                                   /game bomber up 3), again every so often
// where <round> is a random hex number naming the round. If the referee goes silent, the round is off, unrated.

#include "game.hpp"

#include "color.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <format>
#include <map>
#include <optional>
#include <random>
#include <vector>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;

    constexpr std::string_view game_name = "bomber";
    constexpr auto join_time = 20s;
    constexpr std::size_t min_players = 2;
    constexpr std::size_t max_players = 16;

    // The referee's clock: steps of the game.
    constexpr auto step_time = 50ms;
    constexpr int steps_per_second = 20;
    constexpr int countdown_steps = 3 * steps_per_second;
    // When the walls start closing in, after the countdown, and how often a block falls then.
    constexpr int hurry_steps = 120 * steps_per_second;
    constexpr int hurry_every = 4;
    // Once one player (or none) is left, the round ends after a moment, for the last flames to be seen.
    constexpr int end_delay_steps = steps_per_second;

    // Positions are in 24ths of a tile; speeds in those per step.
    constexpr int tile = 24;
    constexpr int base_speed = 3;
    constexpr int max_speed = 4; // power-ups, each one more
    constexpr int start_bombs = 1;
    constexpr int max_bombs = 8;
    constexpr int start_fire = 2;
    constexpr int max_fire = 8;
    constexpr int fuse_steps = 50;
    constexpr int flame_steps = 10;
    // Of the tiles that can have one, how many get a crate, and how many crates hide a power-up (in percent).
    constexpr int crate_percent = 70;
    constexpr int powerup_percent = 35;

    // How often things are told again, as packets get lost.
    constexpr auto open_interval = 1s;
    constexpr auto roster_interval = 1s;
    constexpr auto join_interval = 1s;
    constexpr auto input_interval = 300ms;
    constexpr int end_repeats = 5;
    constexpr auto end_interval = 400ms;
    // Nothing from the referee for so long, and the round is off.
    constexpr auto referee_timeout = 6s;
    // A terminal's walk that gets nowhere for so long stops.
    constexpr auto walk_stuck = 1500ms;

    constexpr int killer_none = -1;
    constexpr int killer_walls = -2;

    std::optional<std::int64_t> parse_int(std::string_view s, int base = 10) {
        std::int64_t value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size()) {
            return std::nullopt;
        }
        return value;
    }

    std::optional<std::uint64_t> parse_hex(std::string_view s) {
        std::uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, 16);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size()) {
            return std::nullopt;
        }
        return value;
    }

    // Takes the next field of s, up to a separator, and removes it with the separator.
    std::string_view next_field(std::string_view& s, char separator = ' ') {
        const auto at = s.find(separator);
        const std::string_view field = s.substr(0, at);
        s.remove_prefix(at == std::string_view::npos ? s.size() : at + 1);
        return field;
    }

    std::string plural(std::int64_t n, std::string_view word) {
        return std::format("{} {}{}", n, word, n == 1 ? "" : "s");
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

    // The size of the map for so many players: odd, with walls all around and a pillar on every other tile.
    std::pair<int, int> map_size(std::size_t players) {
        if (players <= 4) {
            return {15, 13};
        }
        if (players <= 8) {
            return {19, 15};
        }
        return {25, 19};
    }

    // Where players start, the best spread first: the corners, the middles of the sides, then their quarters.
    std::vector<int> spawns(int w, int h) {
        const int r = w - 2;
        const int b = h - 2;
        const std::array<std::pair<int, int>, 16> points {{
            {1, 1},
            {r, b},
            {r, 1},
            {1, b}, // corners
            {w / 2, 1},
            {w / 2, b},
            {1, h / 2},
            {r, h / 2}, // middles
            {w / 4, 1},
            {3 * w / 4, b},
            {3 * w / 4, 1},
            {w / 4, b}, // quarters
            {1, h / 4},
            {r, 3 * h / 4},
            {r, h / 4},
            {1, 3 * h / 4},
        }};
        std::vector<int> out;
        for (const auto& [x, y] : points) {
            out.push_back(y * w + x);
        }
        return out;
    }

    // The tiles inside the outer walls, from the outside in, clockwise: where the walls fall in a hurry.
    std::vector<int> spiral(int w, int h) {
        std::vector<int> out;
        int left = 1, top = 1, right = w - 2, bottom = h - 2;
        while (left <= right && top <= bottom) {
            for (int x = left; x <= right; ++x) {
                out.push_back(top * w + x);
            }
            for (int y = top + 1; y <= bottom; ++y) {
                out.push_back(y * w + right);
            }
            if (top < bottom) {
                for (int x = right - 1; x >= left; --x) {
                    out.push_back(bottom * w + x);
                }
            }
            if (left < right) {
                for (int y = bottom - 1; y > top; --y) {
                    out.push_back(y * w + left);
                }
            }
            ++left;
            ++top;
            --right;
            --bottom;
        }
        return out;
    }

    class Bomber final : public Game {
    public:
        Bomber(Chat& chat, Screen& terminal, Games& games) :
            chat_(chat),
            terminal_(terminal),
            games_(games),
            rng_(std::random_device {}()) {
        }

        ~Bomber() override {
            chat_.set_fast_ticks(false);
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "Bomberman for everyone: blow up crates and each other, the last one standing wins";
        }

        void start() override {
            if (phase_ == Phase::Lobby) {
                join();
                return;
            }
            if (phase_ == Phase::Match) {
                chat_.notice(me_ ? "💣 You are in a round of bomber: /game help bomber for the keys."
                             : terminal_mode_
                                 ? "💣 A round of bomber is on: wait for it to end (/game bomber map shows it)."
                                 : "💣 A round of bomber is on: wait for it to end (the window shows it).");
                return;
            }
            std::uint64_t round = 0;
            do {
                round = rng_();
            } while (round == 0);
            enter_lobby(round, chat_.id(), chat_.name());
            lobby_until_ = clock::now() + join_time;
            lobby_ = {chat_.id()};
            joined_ = true;
            send_open();
            publish();
        }

        void receive(std::uint64_t sender, std::string_view name, std::string_view text) override {
            names_[sender] = std::string(name);
            const std::string_view event = next_field(text);
            const auto round = parse_hex(next_field(text));
            if (!round || *round == 0) {
                return;
            }
            if (event == "open") {
                on_open(sender, name, *round, text);
            } else if (event == "off") {
                if (phase_ == Phase::Lobby && *round == round_ && sender == referee_) {
                    chat_.notice(std::format("💣 Not enough players joined {}'s round of bomber: it is off.",
                                             chat_.colored_name(sender, name)));
                    phase_ = Phase::None;
                    publish();
                }
            } else if (event == "join") {
                on_join(sender, name, *round);
            } else if (event == "roster") {
                on_roster(sender, name, *round, text);
            } else if (event == "s") {
                if (phase_ == Phase::Match && *round == round_ && sender == referee_ && !sim_) {
                    last_heard_ = clock::now();
                    apply_state(text);
                }
            } else if (event == "end") {
                if (phase_ == Phase::Match && *round == round_ && sender == referee_ && !sim_) {
                    std::vector<int> places;
                    while (!text.empty()) {
                        const auto place = parse_int(next_field(text));
                        if (!place || *place < 0 || *place >= static_cast<std::int64_t>(max_players)) {
                            return;
                        }
                        places.push_back(static_cast<int>(*place));
                    }
                    if (places.size() == roster_.size()) {
                        finish(places);
                    }
                }
            } else if (event == "in") {
                on_input(sender, *round, text);
            }
        }

        void message(std::uint64_t, std::string_view, std::string_view) override {
        }

        void tick() override {
            const auto now = clock::now();
            switch (phase_) {
            case Phase::None:
                break;
            case Phase::Lobby:
                if (referee_ == chat_.id()) {
                    if (now >= lobby_until_) {
                        if (lobby_.size() >= min_players) {
                            begin_match();
                        } else {
                            chat_.notice("💣 Nobody else joined your round of bomber: it is off.");
                            send(std::format("off {:x}", round_));
                            phase_ = Phase::None;
                            publish();
                        }
                        return;
                    }
                    if (now >= next_open_) {
                        send_open();
                        publish();
                    }
                } else {
                    if (now - last_heard_ > referee_timeout) {
                        chat_.notice("💣 The round of bomber is off: its referee is gone.");
                        phase_ = Phase::None;
                        publish();
                        return;
                    }
                    if (joined_ && std::ranges::find(lobby_, chat_.id()) == lobby_.end() && now >= next_join_) {
                        send(std::format("join {:x}", round_));
                        next_join_ = now + join_interval;
                    }
                }
                break;
            case Phase::Match:
                if (sim_) {
                    referee_tick(now);
                    return;
                }
                if (now - last_heard_ > referee_timeout) {
                    chat_.notice("💣 The round of bomber is off: its referee is gone. Nobody wins it.");
                    over(std::string("The referee left: nobody wins."));
                    return;
                }
                if (me_ && seen_[*me_].alive && now >= next_input_) {
                    send_input();
                }
                break;
            case Phase::Over:
                // The referee tells how it ended a few times.
                if (end_sent_ < end_repeats && now >= next_end_) {
                    send_end();
                }
                break;
            }
        }

        bool command(std::string_view args) override {
            const std::string_view word = next_field(args);
            if (word == "join" || word == "play") {
                if (phase_ == Phase::None || phase_ == Phase::Over) {
                    start();
                } else {
                    join();
                }
                return true;
            }
            if (word == "go" || word == "now") {
                go_now();
                return true;
            }
            if (word == "key") {
                key(args);
                return true;
            }
            if (word == "bomb" || word == "b" || word == "x") {
                bomb();
                return true;
            }
            if (word == "map" || word == "show") {
                if (phase_ == Phase::Match || phase_ == Phase::Over) {
                    draw();
                } else {
                    chat_.notice("💣 No round of bomber is being played: /game bomber starts one.");
                }
                return true;
            }
            if (word == "stop") {
                walk_.reset();
                key("n");
                return true;
            }
            if (const char dir = direction(word); dir != 0) {
                std::int64_t tiles = 1;
                if (!args.empty()) {
                    const auto n = parse_int(args);
                    if (!n || *n < 1 || *n > 30) {
                        chat_.notice("💣 Walk 1 to 30 tiles, like /game bomber up 3.");
                        return true;
                    }
                    tiles = *n;
                }
                walk(dir, static_cast<int>(tiles));
                return true;
            }
            return false;
        }

        std::vector<Help> help() const override {
            return {
                {"", "start a round: everyone has 20 seconds to join (or join the one that is open)"},
                {"join", "join the round that is open"},
                {"go", "start the round you opened now, without waiting (2 players at least)"},
                {"up|down|left|right [N]", "walk N tiles (1 without a number): for the terminal; in the window, "
                                           "the arrows or WASD"},
                {"bomb", "drop a bomb where you stand: in the window, Space"},
                {"stop", "stop walking"},
                {"map", "show the map (in the terminal)"},
            };
        }

    private:
        enum class Phase { None, Lobby, Match, Over };

        // A player, as the referee's last state says.
        struct Seen {
            int x = 0;
            int y = 0;
            bool alive = true;
            int bombs = start_bombs;
            int fire = start_fire;
            int speed = 0;
            int killer = killer_none;
        };

        // The referee's own game.
        struct SimPlayer {
            int x = 0;
            int y = 0;
            bool alive = true;
            int bombs = start_bombs;
            int fire = start_fire;
            int speed = 0;
            char key = 'n';
            std::int64_t asked = 0; // bombs asked for
            int target = -1;        // a tile to stop at, for a walk
            int died = -1;          // the step
            int killer = killer_none;
        };
        struct Bomb {
            int cell = 0;
            int owner = 0;
            int fuse = fuse_steps;
            int fire = start_fire;
        };
        struct Sim {
            int w = 0;
            int h = 0;
            std::string grid;        // # + . b f s
            std::string hidden;      // under the crates: a power-up, or .
            std::vector<int> flame;  // steps left
            std::vector<int> flamer; // whose bomb
            std::vector<Bomb> bombs;
            std::vector<SimPlayer> players;
            std::vector<int> spiral;
            std::size_t fallen = 0;
            int step = 0;
            int end_at = -1;
            clock::time_point next_step;
            clock::time_point next_roster;
        };

        static char direction(std::string_view word) {
            if (word == "up" || word == "u" || word == "north") {
                return 'u';
            }
            if (word == "down" || word == "d" || word == "south") {
                return 'd';
            }
            if (word == "left" || word == "l" || word == "west") {
                return 'l';
            }
            if (word == "right" || word == "r" || word == "east") {
                return 'r';
            }
            return 0;
        }

        std::string name_of(std::uint64_t id) const {
            if (id == chat_.id()) {
                return chat_.name();
            }
            if (const auto it = names_.find(id); it != names_.end()) {
                return it->second;
            }
            for (const auto& [peer, name] : chat_.people()) {
                if (peer == id) {
                    return name;
                }
            }
            return "Somebody";
        }

        std::string colored(std::uint64_t id) const {
            return chat_.colored_name(id, name_of(id));
        }

        // --- The lobby.

        void enter_lobby(std::uint64_t round, std::uint64_t referee, std::string_view referee_name) {
            phase_ = Phase::Lobby;
            round_ = round;
            referee_ = referee;
            names_[referee] = std::string(referee_name);
            lobby_.clear();
            joined_ = false;
            said_in_ = false;
            sim_.reset();
            roster_.clear();
            seen_.clear();
            me_.reset();
            walk_.reset();
            result_.clear();
            places_.clear();
            frame_ = -1;
            last_heard_ = clock::now();
            next_join_ = {};
            chat_.notice(std::format("💣 Bomber, started by {}! /game bomber join within {} seconds to play: bombs "
                                     "blow up crates and players, the last one standing wins (/game help bomber).",
                                     colored(referee), join_time.count()));
        }

        void join() {
            if (phase_ != Phase::Lobby) {
                chat_.notice("💣 No round of bomber is open: /game bomber starts one.");
                return;
            }
            if (joined_) {
                chat_.notice("💣 You are in: the round starts when the time to join is up.");
                return;
            }
            joined_ = true;
            if (referee_ == chat_.id()) {
                return;
            }
            send(std::format("join {:x}", round_));
            next_join_ = clock::now() + join_interval;
            chat_.notice("💣 Joining the round of bomber…");
            publish();
        }

        void on_open(std::uint64_t sender, std::string_view name, std::uint64_t round, std::string_view text) {
            const auto seconds = parse_int(next_field(text));
            if (!seconds || *seconds < 0) {
                return;
            }
            std::vector<std::uint64_t> ids;
            while (!text.empty()) {
                const auto id = parse_hex(next_field(text));
                if (!id || ids.size() >= max_players) {
                    return;
                }
                ids.push_back(*id);
            }
            if (phase_ == Phase::Lobby && round != round_) {
                // Two rounds opened at once: everybody plays the one with the lowest number.
                if (round > round_) {
                    return;
                }
                const bool was_joined = joined_;
                if (referee_ == chat_.id()) {
                    send(std::format("off {:x}", round_));
                }
                enter_lobby(round, sender, name);
                if (was_joined) {
                    join();
                }
            } else if (phase_ == Phase::Match && round != round_) {
                return;
            } else if (phase_ == Phase::Match || (phase_ == Phase::Over && round == round_)) {
                return;
            } else if (phase_ != Phase::Lobby) {
                enter_lobby(round, sender, name);
                terminal_.bell();
            }
            if (sender != referee_) {
                return;
            }
            last_heard_ = clock::now();
            for (const std::uint64_t id : ids) {
                if (std::ranges::find(lobby_, id) == lobby_.end() && id != referee_) {
                    chat_.notice(std::format("💣 {} joins the round of bomber.", colored(id)));
                }
            }
            lobby_ = std::move(ids);
            lobby_left_ = static_cast<int>(*seconds);
            if (joined_ && std::ranges::find(lobby_, chat_.id()) != lobby_.end() && !said_in_) {
                said_in_ = true;
                chat_.notice(std::format("💣 You are in! The round starts in {}.", plural(*seconds, "second")));
            }
            publish();
        }

        void on_join(std::uint64_t sender, std::string_view name, std::uint64_t round) {
            if (phase_ != Phase::Lobby || round != round_ || referee_ != chat_.id()) {
                return;
            }
            if (std::ranges::find(lobby_, sender) != lobby_.end()) {
                send_open();
                return;
            }
            if (lobby_.size() >= max_players) {
                return;
            }
            lobby_.push_back(sender);
            chat_.notice(std::format("💣 {} joins the round of bomber.", chat_.colored_name(sender, name)));
            send_open();
            publish();
        }

        void send_open() {
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(lobby_until_ - clock::now());
            lobby_left_ = static_cast<int>(std::max<long long>(0, left.count()));
            std::string ids;
            for (const std::uint64_t id : lobby_) {
                ids += std::format(" {:x}", id);
            }
            send(std::format("open {:x} {}{}", round_, lobby_left_, ids));
            next_open_ = clock::now() + open_interval;
        }

        void go_now() {
            if (phase_ != Phase::Lobby || referee_ != chat_.id()) {
                chat_.notice(phase_ == Phase::Lobby ? "💣 Only who opened the round can start it early."
                                                    : "💣 No round of bomber is open: /game bomber opens one.");
                return;
            }
            if (lobby_.size() < min_players) {
                chat_.notice("💣 It takes 2 players at least: wait for somebody to join.");
                return;
            }
            begin_match();
        }

        // --- The match: the referee's side.

        void begin_match() {
            const auto [w, h] = map_size(lobby_.size());
            Sim sim;
            sim.w = w;
            sim.h = h;
            sim.grid.assign(static_cast<std::size_t>(w * h), '.');
            sim.hidden.assign(sim.grid.size(), '.');
            sim.flame.assign(sim.grid.size(), 0);
            sim.flamer.assign(sim.grid.size(), 0);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (x == 0 || y == 0 || x == w - 1 || y == h - 1 || (x % 2 == 0 && y % 2 == 0)) {
                        sim.grid[static_cast<std::size_t>(y * w + x)] = '#';
                    }
                }
            }
            // Players get the spawns in a random order, and room to move away from their first bomb.
            auto starts = spawns(w, h);
            starts.resize(lobby_.size());
            std::ranges::shuffle(starts, rng_);
            std::vector<bool> keep_clear(sim.grid.size(), false);
            for (const int cell : starts) {
                const int sx = cell % w, sy = cell / w;
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        if (std::abs(x - sx) + std::abs(y - sy) <= 2) {
                            keep_clear[static_cast<std::size_t>(y * w + x)] = true;
                        }
                    }
                }
                SimPlayer p;
                p.x = sx * tile + tile / 2;
                p.y = sy * tile + tile / 2;
                sim.players.push_back(p);
            }
            std::uniform_int_distribution<int> percent(0, 99);
            for (std::size_t i = 0; i < sim.grid.size(); ++i) {
                if (sim.grid[i] == '.' && !keep_clear[i] && percent(rng_) < crate_percent) {
                    sim.grid[i] = '+';
                    if (percent(rng_) < powerup_percent) {
                        const int kind = percent(rng_);
                        sim.hidden[i] = kind < 40 ? 'b' : kind < 80 ? 'f' : 's';
                    }
                }
            }
            sim.spiral = spiral(w, h);
            sim.next_step = clock::now();
            sim.next_roster = clock::now();

            roster_ = lobby_;
            begin_view(w, h);
            sim_ = std::move(sim);
            chat_.set_fast_ticks(true);
            send_roster();
            chat_.notice(std::format("💣 The round of bomber starts, with {}!", roster_names()));
        }

        void send_roster() {
            std::string ids;
            for (const std::uint64_t id : roster_) {
                ids += std::format(" {:x}", id);
            }
            send(std::format("roster {:x} {} {}{}", round_, w_, h_, ids));
            sim_->next_roster = clock::now() + roster_interval;
        }

        void referee_tick(clock::time_point now) {
            Sim& s = *sim_;
            if (now >= s.next_roster) {
                send_roster();
            }
            // Never more than a few steps at once, when the clock jumped.
            if (now - s.next_step > 5 * step_time) {
                s.next_step = now;
            }
            bool stepped = false;
            while (now >= s.next_step) {
                step(s);
                s.next_step += step_time;
                stepped = true;
                if (s.end_at >= 0 && s.step >= s.end_at) {
                    break;
                }
            }
            if (!stepped) {
                return;
            }
            const std::string state = encode_state(s);
            send("s " + state);
            // The referee sees what it sends, as everybody: skipping "s <round> ".
            std::string_view text = state;
            next_field(text);
            apply_state(text);
            if (s.end_at >= 0 && s.step >= s.end_at) {
                places_ = placings(s);
                end_sent_ = 0;
                send_end();
                finish(places_);
            }
        }

        static std::vector<int> placings(const Sim& s) {
            // Who lasted longer is better placed; who is still standing, the best.
            constexpr int standing = 1 << 30;
            std::vector<int> places;
            for (const SimPlayer& p : s.players) {
                const int lasted = p.alive ? standing : p.died;
                places.push_back(static_cast<int>(std::ranges::count_if(s.players, [&](const SimPlayer& o) {
                    return (o.alive ? standing : o.died) > lasted;
                })));
            }
            return places;
        }

        void send_end() {
            std::string places;
            for (const int p : places_) {
                places += std::format(" {}", p);
            }
            send(std::format("end {:x}{}", round_, places));
            ++end_sent_;
            next_end_ = clock::now() + end_interval;
        }

        void on_input(std::uint64_t sender, std::uint64_t round, std::string_view text) {
            if (!sim_ || phase_ != Phase::Match || round != round_) {
                return;
            }
            const auto at = std::ranges::find(roster_, sender);
            if (at == roster_.end()) {
                return;
            }
            const std::string_view key = next_field(text);
            const auto asked = parse_int(next_field(text));
            const auto target = text.empty() ? std::optional<std::int64_t>(-1) : parse_int(text);
            if (key.size() != 1 || std::string_view("udlrn").find(key[0]) == std::string_view::npos || !asked ||
                !target) {
                return;
            }
            control(static_cast<std::size_t>(at - roster_.begin()), key[0], *asked, static_cast<int>(*target));
        }

        // A player's keys, from their packet or (ours) right away.
        void control(std::size_t index, char key, std::int64_t asked, int target) {
            SimPlayer& p = sim_->players[index];
            p.key = key;
            p.target = target >= 0 && target < sim_->w * sim_->h ? target : -1;
            if (asked > p.asked) {
                p.asked = asked;
                drop_bomb(*sim_, static_cast<int>(index));
            }
        }

        static int cell_of(const Sim& s, const SimPlayer& p) {
            return (p.y / tile) * s.w + p.x / tile;
        }

        static bool bomb_at(const Sim& s, int cell) {
            return std::ranges::any_of(s.bombs, [&](const Bomb& b) {
                return b.cell == cell;
            });
        }

        // Whether a player standing on here can walk onto cell: not into walls, crates and bombs, but off the bomb
        // they stand on.
        static bool passable(const Sim& s, int cell, int here) {
            const char c = s.grid[static_cast<std::size_t>(cell)];
            return c != '#' && c != '+' && (cell == here || !bomb_at(s, cell));
        }

        static void drop_bomb(Sim& s, int index) {
            SimPlayer& p = s.players[static_cast<std::size_t>(index)];
            if (!p.alive || s.step < countdown_steps) {
                return;
            }
            const int cell = cell_of(s, p);
            const auto mine = std::ranges::count(s.bombs, index, &Bomb::owner);
            if (mine >= p.bombs || bomb_at(s, cell)) {
                return;
            }
            s.bombs.push_back({cell, index, fuse_steps, p.fire});
        }

        // Moves a player along the key they hold. They first get to the middle of their row (or column), so that
        // turning into a corridor just works, then go on as far as nothing is in the way.
        static void move(Sim& s, SimPlayer& p) {
            if (p.key == 'n') {
                return;
            }
            const int dx = (p.key == 'r') - (p.key == 'l');
            const int dy = (p.key == 'd') - (p.key == 'u');
            const int v = base_speed + p.speed;
            const int col = p.x / tile, row = p.y / tile;
            const int cx = col * tile + tile / 2, cy = row * tile + tile / 2;
            const int here = row * s.w + col;
            // How far along one axis: up to the middle of the tile when the next one is blocked (or is where a walk
            // stops), never back.
            const auto advance = [&](int pos, int center, int d, int next) {
                const int to = pos + d * v;
                const bool blocked = here == p.target || !passable(s, next, here);
                if (!blocked) {
                    return to;
                }
                if (d > 0) {
                    return pos < center ? std::min(to, center) : pos;
                }
                return pos > center ? std::max(to, center) : pos;
            };
            if (dx != 0) {
                if (p.y != cy) {
                    p.y += std::clamp(cy - p.y, -v, v);
                    return;
                }
                p.x = advance(p.x, cx, dx, here + dx);
            } else {
                if (p.x != cx) {
                    p.x += std::clamp(cx - p.x, -v, v);
                    return;
                }
                p.y = advance(p.y, cy, dy, here + dy * s.w);
            }
        }

        // The flames of the bombs whose time has come, and of those they set off.
        static void explode(Sim& s) {
            std::vector<int> crates;
            for (;;) {
                const auto it = std::ranges::find_if(s.bombs, [](const Bomb& b) {
                    return b.fuse <= 0;
                });
                if (it == s.bombs.end()) {
                    break;
                }
                const Bomb bomb = *it;
                s.bombs.erase(it);
                const auto burn = [&](int cell) {
                    s.flame[static_cast<std::size_t>(cell)] = flame_steps;
                    s.flamer[static_cast<std::size_t>(cell)] = bomb.owner;
                };
                burn(bomb.cell);
                for (const int d : {1, -1, s.w, -s.w}) {
                    for (int i = 1, cell = bomb.cell + d; i <= bomb.fire; ++i, cell += d) {
                        char& c = s.grid[static_cast<std::size_t>(cell)];
                        if (c == '#') {
                            break;
                        }
                        if (c == '+') {
                            burn(cell);
                            crates.push_back(cell);
                            break;
                        }
                        burn(cell);
                        if (c == 'b' || c == 'f' || c == 's') {
                            c = '.';
                            break;
                        }
                        const auto other = std::ranges::find(s.bombs, cell, &Bomb::cell);
                        if (other != s.bombs.end()) {
                            other->fuse = 0;
                            break;
                        }
                    }
                }
            }
            // Crates break once all the flames are out of the way, and show what they hid.
            for (const int cell : crates) {
                s.grid[static_cast<std::size_t>(cell)] = s.hidden[static_cast<std::size_t>(cell)];
            }
        }

        void step(Sim& s) {
            ++s.step;
            const bool playing = s.step > countdown_steps;
            if (playing) {
                for (SimPlayer& p : s.players) {
                    if (!p.alive) {
                        continue;
                    }
                    move(s, p);
                    char& c = s.grid[static_cast<std::size_t>(cell_of(s, p))];
                    if (c == 'b') {
                        p.bombs = std::min(max_bombs, p.bombs + 1);
                    } else if (c == 'f') {
                        p.fire = std::min(max_fire, p.fire + 1);
                    } else if (c == 's') {
                        p.speed = std::min(max_speed, p.speed + 1);
                    }
                    if (c == 'b' || c == 'f' || c == 's') {
                        c = '.';
                    }
                }
            }
            for (int& f : s.flame) {
                f = std::max(0, f - 1);
            }
            for (Bomb& b : s.bombs) {
                --b.fuse;
            }
            explode(s);
            for (SimPlayer& p : s.players) {
                const auto cell = static_cast<std::size_t>(cell_of(s, p));
                if (p.alive && s.flame[cell] > 0) {
                    p.alive = false;
                    p.died = s.step;
                    p.killer = s.flamer[cell];
                }
            }
            // The hurry: the walls close in, from the outside.
            if (s.step >= countdown_steps + hurry_steps &&
                (s.step - countdown_steps - hurry_steps) % hurry_every == 0) {
                while (s.fallen < s.spiral.size() && s.grid[static_cast<std::size_t>(s.spiral[s.fallen])] == '#') {
                    ++s.fallen;
                }
                if (s.fallen < s.spiral.size()) {
                    const int cell = s.spiral[s.fallen++];
                    s.grid[static_cast<std::size_t>(cell)] = '#';
                    s.hidden[static_cast<std::size_t>(cell)] = '.';
                    s.flame[static_cast<std::size_t>(cell)] = 0;
                    std::erase_if(s.bombs, [&](const Bomb& b) {
                        return b.cell == cell;
                    });
                    for (SimPlayer& p : s.players) {
                        if (p.alive && cell_of(s, p) == cell) {
                            p.alive = false;
                            p.died = s.step;
                            p.killer = killer_walls;
                        }
                    }
                }
            }
            const auto alive = std::ranges::count(s.players, true, &SimPlayer::alive);
            if (alive <= 1 && s.end_at < 0) {
                s.end_at = s.step + end_delay_steps;
            }
        }

        std::string encode_state(const Sim& s) const {
            std::string map = s.grid;
            for (std::size_t i = 0; i < map.size(); ++i) {
                if (s.flame[i] > 0) {
                    map[i] = '*';
                }
            }
            for (const Bomb& b : s.bombs) {
                map[static_cast<std::size_t>(b.cell)] = 'o';
            }
            char phase = 'p';
            int seconds = 0;
            if (s.step <= countdown_steps) {
                phase = 'c';
                seconds = (countdown_steps - s.step + steps_per_second - 1) / steps_per_second;
            } else if (s.step < countdown_steps + hurry_steps) {
                seconds = (countdown_steps + hurry_steps - s.step + steps_per_second - 1) / steps_per_second;
            } else {
                phase = 'h';
            }
            std::string out = std::format("{:x} {} {} {} {}", round_, s.step, phase, seconds, map);
            for (const SimPlayer& p : s.players) {
                out +=
                    std::format(" {},{},{},{},{},{},{}", p.x, p.y, p.alive ? 1 : 0, p.bombs, p.fire, p.speed, p.killer);
            }
            return out;
        }

        // --- The match: everybody's side.

        void on_roster(std::uint64_t sender, std::string_view name, std::uint64_t round, std::string_view text) {
            if (phase_ == Phase::Match && round == round_) {
                last_heard_ = clock::now();
                return;
            }
            if (phase_ == Phase::Match || (phase_ == Phase::Over && round == round_) || sim_) {
                return;
            }
            const auto w = parse_int(next_field(text));
            const auto h = parse_int(next_field(text));
            if (!w || !h || *w < 5 || *h < 5 || *w > 31 || *h > 31 || *w % 2 == 0 || *h % 2 == 0) {
                return;
            }
            std::vector<std::uint64_t> ids;
            while (!text.empty()) {
                const auto id = parse_hex(next_field(text));
                if (!id || ids.size() >= max_players) {
                    return;
                }
                ids.push_back(*id);
            }
            if (ids.size() < min_players) {
                return;
            }
            const bool was_lobby = phase_ == Phase::Lobby && round == round_;
            round_ = round;
            referee_ = sender;
            names_[sender] = std::string(name);
            roster_ = std::move(ids);
            begin_view(static_cast<int>(*w), static_cast<int>(*h));
            chat_.set_fast_ticks(true);
            last_heard_ = clock::now();
            if (me_) {
                chat_.notice(std::format("💣 The round of bomber starts, with {}! {}", roster_names(),
                                         terminal_mode_ ? "/game bomber up, down, left, right [N] walks, "
                                                          "/game bomber bomb drops a bomb, /game bomber map shows it."
                                                        : "Arrows or WASD move, Space drops a bomb."));
                terminal_.bell();
            } else if (was_lobby) {
                chat_.notice(std::format("💣 The round of bomber starts, with {}: {} shows it.", roster_names(),
                                         terminal_mode_ ? "/game bomber map" : "the window"));
            }
        }

        // A new match, with roster_: everybody is where the first state puts them.
        void begin_view(int w, int h) {
            phase_ = Phase::Match;
            w_ = w;
            h_ = h;
            grid_.assign(static_cast<std::size_t>(w * h), '.');
            seen_.assign(roster_.size(), Seen {});
            me_.reset();
            const auto at = std::ranges::find(roster_, chat_.id());
            if (at != roster_.end()) {
                me_ = static_cast<std::size_t>(at - roster_.begin());
            }
            key_ = 'n';
            asked_ = 0;
            walk_.reset();
            frame_ = -1;
            sub_ = 'c';
            seconds_ = 0;
            result_.clear();
            places_.clear();
            end_sent_ = end_repeats;
        }

        std::string roster_names() const {
            std::string out;
            for (std::size_t i = 0; i < roster_.size(); ++i) {
                out += std::format("{}{}", i == 0 ? "" : i + 1 == roster_.size() ? " and " : ", ", colored(roster_[i]));
            }
            return out;
        }

        void apply_state(std::string_view text) {
            const auto frame = parse_int(next_field(text));
            const std::string_view phase = next_field(text);
            const auto seconds = parse_int(next_field(text));
            const std::string_view map = next_field(text);
            if (!frame || *frame <= frame_ || phase.size() != 1 || !seconds ||
                map.size() != static_cast<std::size_t>(w_ * h_) ||
                map.find_first_not_of("#+.ofbs*") != std::string_view::npos) {
                return;
            }
            std::vector<Seen> seen;
            while (!text.empty()) {
                std::string_view fields = next_field(text);
                std::array<std::int64_t, 7> v {};
                for (auto& value : v) {
                    const auto n = parse_int(next_field(fields, ','));
                    if (!n) {
                        return;
                    }
                    value = *n;
                }
                const auto limit = static_cast<std::int64_t>(std::max(w_, h_) * tile);
                if (v[0] < 0 || v[1] < 0 || v[0] >= limit || v[1] >= limit) {
                    return;
                }
                seen.push_back({static_cast<int>(v[0]), static_cast<int>(v[1]), v[2] != 0,
                                static_cast<int>(std::clamp<std::int64_t>(v[3], 0, max_bombs)),
                                static_cast<int>(std::clamp<std::int64_t>(v[4], 0, max_fire)),
                                static_cast<int>(std::clamp<std::int64_t>(v[5], 0, max_speed)),
                                static_cast<int>(std::clamp<std::int64_t>(v[6], killer_walls,
                                                                          static_cast<std::int64_t>(max_players)))});
            }
            if (seen.size() != roster_.size()) {
                return;
            }
            frame_ = *frame;
            grid_ = std::string(map);
            const char was = sub_;
            sub_ = phase[0];
            seconds_ = static_cast<int>(*seconds);
            // Who was blown up since the last state.
            for (std::size_t i = 0; i < seen.size(); ++i) {
                if (seen_[i].alive && !seen[i].alive && frame_ > 0) {
                    obituary(i, seen[i].killer);
                }
            }
            seen_ = std::move(seen);
            if (was == 'c' && sub_ == 'p') {
                chat_.notice("💣 Go!");
                if (terminal_mode_ && me_) {
                    draw();
                }
            }
            if (was == 'p' && sub_ == 'h') {
                chat_.notice("⚠ Hurry up! The walls are closing in.");
            }
            follow_walk();
            publish();
        }

        void obituary(std::size_t victim, int killer) {
            const std::string who = colored(roster_[victim]);
            if (killer == killer_walls) {
                chat_.notice(std::format("🧱 {} was crushed by the walls.", who));
            } else if (killer == static_cast<int>(victim)) {
                chat_.notice(std::format("💥 {} blew themselves up.", who));
            } else if (killer >= 0 && static_cast<std::size_t>(killer) < roster_.size()) {
                chat_.notice(
                    std::format("💥 {} was blown up by {}.", who, colored(roster_[static_cast<std::size_t>(killer)])));
            } else {
                chat_.notice(std::format("💥 {} was blown up.", who));
            }
            if (me_ && *me_ == victim) {
                walk_.reset();
                terminal_.bell();
                if (terminal_mode_) {
                    chat_.notice("   You are out: the window, or /game bomber map, shows how the others do.");
                }
            }
        }

        // The round is over: where everybody finished, which is rated.
        void finish(const std::vector<int>& places) {
            places_ = places;
            std::vector<Placing> placings;
            std::string winners;
            std::string plain_winners;
            std::size_t first = 0;
            for (std::size_t i = 0; i < roster_.size(); ++i) {
                const std::string name = name_of(roster_[i]);
                placings.push_back({roster_[i], name, places[i]});
                if (places[i] == 0) {
                    ++first;
                    winners += std::format("{}{}", winners.empty() ? "" : " and ", colored(roster_[i]));
                    plain_winners += std::format("{}{}", plain_winners.empty() ? "" : " and ", name);
                }
            }
            std::string result;
            if (first == roster_.size()) {
                result = "Nobody is left standing: a draw!";
                chat_.notice("💣 Bomber is over: everybody went up at once, a draw!");
            } else {
                for (std::size_t i = 0; i < roster_.size(); ++i) {
                    if (places[i] == 0) {
                        games_.add_win(game_name, roster_[i], name_of(roster_[i]));
                    }
                }
                result = std::format("{} win{}!", plain_winners, first == 1 ? "s" : "");
                chat_.notice(std::format("🏆 {} win{} bomber{}!", winners, first == 1 ? "s" : "",
                                         first == 1 ? ", the last one standing" : ""));
            }
            games_.rate(game_name, placings);
            over(std::move(result));
        }

        void over(std::string result) {
            phase_ = Phase::Over;
            sim_.reset();
            result_ = std::move(result);
            walk_.reset();
            chat_.set_fast_ticks(false);
            publish();
        }

        // --- Our keys.

        void key(std::string_view which) {
            const char k = which.size() == 1 && std::string_view("udlrn").find(which[0]) != std::string_view::npos
                               ? which[0]
                               : direction(which);
            if (k == 0) {
                return;
            }
            if (!me_ || phase_ != Phase::Match) {
                return;
            }
            walk_.reset();
            key_ = k;
            target_ = -1;
            send_input();
        }

        void bomb() {
            if (!me_ || phase_ != Phase::Match) {
                chat_.notice("💣 You are not playing a round of bomber: /game bomber starts one.");
                return;
            }
            if (!seen_[*me_].alive) {
                return;
            }
            ++asked_;
            send_input();
        }

        // A terminal's walk: holds the key until we stand in the middle of the tile so many away, or get stuck.
        void walk(char dir, int tiles) {
            if (!me_ || phase_ != Phase::Match) {
                chat_.notice("💣 You are not playing a round of bomber: /game bomber starts one.");
                return;
            }
            const Seen& me = seen_[*me_];
            if (!me.alive) {
                chat_.notice("💣 You are out of this round.");
                return;
            }
            const int dx = (dir == 'r') - (dir == 'l');
            const int dy = (dir == 'd') - (dir == 'u');
            const int x = std::clamp(me.x / tile + dx * tiles, 0, w_ - 1);
            const int y = std::clamp(me.y / tile + dy * tiles, 0, h_ - 1);
            walk_ = Walk {y * w_ + x, me.x, me.y, clock::now()};
            key_ = dir;
            target_ = walk_->target;
            send_input();
        }

        void follow_walk() {
            if (!walk_ || !me_) {
                return;
            }
            const Seen& me = seen_[*me_];
            const int cell = (me.y / tile) * w_ + me.x / tile;
            const bool there = cell == walk_->target && me.x % tile == tile / 2 && me.y % tile == tile / 2;
            const auto now = clock::now();
            if (me.x != walk_->x || me.y != walk_->y) {
                walk_->x = me.x;
                walk_->y = me.y;
                walk_->moved = now;
            }
            const bool stuck = now - walk_->moved > walk_stuck;
            if (!there && !stuck) {
                return;
            }
            walk_.reset();
            key_ = 'n';
            target_ = -1;
            send_input();
            if (stuck && !there) {
                chat_.notice("💣 Something is in the way.");
            }
            if (terminal_mode_) {
                draw();
            }
        }

        void send_input() {
            next_input_ = clock::now() + input_interval;
            if (sim_) {
                control(*me_, key_, asked_, target_);
                return;
            }
            send(target_ >= 0 ? std::format("in {:x} {} {} {}", round_, key_, asked_, target_)
                              : std::format("in {:x} {} {}", round_, key_, asked_));
        }

        // --- Showing it.

        // The map in the terminal, two characters per tile, with the players by letter.
        void draw() const {
            if (seen_.empty()) {
                return;
            }
            const bool vt = terminal_.colors();
            std::vector<int> who(grid_.size(), -1);
            for (std::size_t i = 0; i < seen_.size(); ++i) {
                if (seen_[i].alive) {
                    who[static_cast<std::size_t>((seen_[i].y / tile) * w_ + seen_[i].x / tile)] = static_cast<int>(i);
                }
            }
            const auto bg = [](int r, int g, int b) {
                return std::format("\x1b[48;2;{};{};{}m", r, g, b);
            };
            std::string out;
            for (int y = 0; y < h_; ++y) {
                out += y == 0 ? "   " : "\n   ";
                for (int x = 0; x < w_; ++x) {
                    const auto cell = static_cast<std::size_t>(y * w_ + x);
                    const char c = grid_[cell];
                    if (who[cell] >= 0) {
                        const auto i = static_cast<std::size_t>(who[cell]);
                        const char letter = static_cast<char>('A' + i);
                        if (vt) {
                            const Color col = color_of_id(roster_[i]);
                            out += std::format("{}\x1b[38;2;0;0;0;1m{}{}\x1b[0m", bg(col.r, col.g, col.b),
                                               me_ && *me_ == i ? '@' : ' ', letter);
                        } else {
                            out += std::format("{}{}", me_ && *me_ == i ? '@' : ' ', letter);
                        }
                        continue;
                    }
                    if (!vt) {
                        out += c == '#'   ? "##"
                               : c == '+' ? "[]"
                               : c == 'o' ? "()"
                               : c == '*' ? "**"
                               : c == 'b' ? "B+"
                               : c == 'f' ? "F+"
                               : c == 's' ? "S+"
                                          : "  ";
                        continue;
                    }
                    switch (c) {
                    case '#':
                        out += bg(110, 116, 128) + "  ";
                        break;
                    case '+':
                        out += bg(150, 98, 50) + "\x1b[38;2;95;60;28m▒▒";
                        break;
                    case 'o':
                        out += bg(46, 110, 58) + "\x1b[38;2;20;20;20;1m()";
                        break;
                    case '*':
                        out += bg(250, 160, 40) + "\x1b[38;2;255;240;120;1m**";
                        break;
                    case 'b':
                    case 'f':
                    case 's':
                        out += bg(46, 110, 58) +
                               std::format("\x1b[38;2;255;230;90;1m{}+", static_cast<char>(c - 'a' + 'A'));
                        break;
                    default:
                        out += bg(46, 110, 58) + "  ";
                    }
                    out += "\x1b[0m";
                }
            }
            chat_.notice(sub_ == 'c'   ? std::format("💣 Bomber starts in {}:", plural(seconds_, "second"))
                         : sub_ == 'h' ? std::string("💣 Bomber, hurry up! The walls are closing in:")
                                       : std::format("💣 Bomber, {} to the hurry:", plural(seconds_, "second")));
            terminal_.print(out);
            for (std::size_t i = 0; i < seen_.size(); ++i) {
                const Seen& p = seen_[i];
                chat_.notice(std::format("   {} {}{}: {}", static_cast<char>('A' + i), colored(roster_[i]),
                                         me_ && *me_ == i ? " (you, @)" : "",
                                         p.alive ? std::format("{} bomb{}, fire {}, speed {}", p.bombs,
                                                               p.bombs == 1 ? "" : "s", p.fire, p.speed + 1)
                                                 : std::string("out")));
            }
            chat_.notice(std::format("   {} wall, {} crate, () bomb, ** flames, B+ F+ S+ power-ups: more bombs, fire, "
                                     "speed; @ is you",
                                     vt ? "grey" : "##", vt ? "▒▒" : "[]"));
        }

        // The round, for a window of its own (see Screen::show_game()).
        void publish() {
            const char* phase = phase_ == Phase::None    ? "none"
                                : phase_ == Phase::Lobby ? "lobby"
                                : phase_ == Phase::Over  ? "over"
                                : sub_ == 'c'            ? "countdown"
                                : sub_ == 'h'            ? "hurry"
                                                         : "play";
            std::string players = "[";
            const auto& ids = phase_ == Phase::Lobby ? lobby_ : roster_;
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const Color c = color_of_id(ids[i]);
                const Seen p = i < seen_.size() ? seen_[i] : Seen {};
                players +=
                    std::format("{}{{\"name\":{},\"color\":\"#{:02x}{:02x}{:02x}\",\"you\":{},\"x\":{},\"y\":{},"
                                "\"alive\":{},\"bombs\":{},\"fire\":{},\"speed\":{},\"place\":{}}}",
                                i == 0 ? "" : ",", json(name_of(ids[i])), c.r, c.g, c.b, ids[i] == chat_.id(), p.x, p.y,
                                p.alive, p.bombs, p.fire, p.speed + 1, i < places_.size() ? places_[i] : -1);
            }
            players += "]";
            const std::string state = std::format(
                "{{\"round\":\"{:x}\",\"phase\":\"{}\",\"referee\":{},\"isReferee\":{},\"joined\":{},\"playing\":{},"
                "\"joinLeft\":{},\"left\":{},\"frame\":{},\"w\":{},\"h\":{},\"unit\":{},\"grid\":{},\"players\":{},"
                "\"result\":{}}}",
                round_, phase, json(phase_ == Phase::None ? std::string() : name_of(referee_)), referee_ == chat_.id(),
                joined_, me_.has_value(), lobby_left_, seconds_, frame_, w_, h_, tile, json(grid_), players,
                json(result_));
            terminal_mode_ = !terminal_.show_game(game_name, state);
        }

        void send(std::string_view event) {
            chat_.send_game(std::format("{} {}", game_name, event));
        }

        Chat& chat_;
        Screen& terminal_;
        Games& games_;
        std::mt19937_64 rng_;

        Phase phase_ = Phase::None;
        std::uint64_t round_ = 0;
        std::uint64_t referee_ = 0;
        clock::time_point last_heard_;
        // Names heard, by sender id.
        std::map<std::uint64_t, std::string> names_;

        // The lobby: who joined (the referee's list), whether we asked to, and the seconds left to.
        std::vector<std::uint64_t> lobby_;
        bool joined_ = false;
        bool said_in_ = false;
        int lobby_left_ = 0;
        clock::time_point lobby_until_;
        clock::time_point next_open_;
        clock::time_point next_join_;

        // The match, as the referee's last state says.
        std::vector<std::uint64_t> roster_;
        int w_ = 0;
        int h_ = 0;
        std::string grid_;
        std::vector<Seen> seen_;
        std::int64_t frame_ = -1;
        char sub_ = 'c';
        int seconds_ = 0;
        // Us among the players, by place in the roster.
        std::optional<std::size_t> me_;
        // Our keys: the one held, the bombs asked for, a tile to stop at.
        char key_ = 'n';
        std::int64_t asked_ = 0;
        int target_ = -1;
        clock::time_point next_input_;
        struct Walk {
            int target = 0;
            int x = 0;
            int y = 0;
            clock::time_point moved;
        };
        std::optional<Walk> walk_;

        // When we are the referee of the match.
        std::optional<Sim> sim_;
        std::vector<int> places_;
        int end_sent_ = end_repeats;
        clock::time_point next_end_;

        std::string result_;
        // Whether the last state could not be shown in a window: the map is drawn with characters then.
        bool terminal_mode_ = true;
    };

} // namespace

std::unique_ptr<Game> make_bomber(Chat& chat, Screen& terminal, Games& games) {
    return std::make_unique<Bomber>(chat, terminal, games);
}

} // namespace zchat::game
