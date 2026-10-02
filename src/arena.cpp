// Arena: a top-down shooter for the whole chat, a deathmatch on a small map of walls and cover, so the others are never
// far. Everyone who joins starts in a corner (or on a side); shots fly in a straight line until they hit a wall or
// somebody, and four hits take a player out, who comes back a few seconds later somewhere away from the others. Med
// kits on the map heal. After three minutes whoever took out the most players wins (the fewest times out breaks a
// tie). Up to 16 can play: the map grows with them. In a window it has a window of its own (see Screen::show_game()),
// played with WASD (or the arrows) to move, the mouse to aim and its button (or Space) to shoot; in a terminal, with
// /game arena move up, /game arena shoot [NAME] and /game arena map.
//
// It is played in real time, like bomber: whoever starts the round is its referee all along, runs it 20 steps a
// second and sends everybody where things are after each step. The players only send what they hold. Game packets
// carry at most 1000 bytes, so the map goes with the roster, and each step only the players and the shots, packed:
// numbers in base 64 (0-9, A-Z, a-z, - and _), fixed widths, no spaces. As Game packets, the referee sends:
//   arena open <round> <seconds> <id>...           the round can be joined for so many seconds more, by these so far
//   arena off <round>                              the round is off (not enough players)
//   arena roster <round> <width> <height> <map> <id>...
//                                                  the round is on, with this map (a character per tile, row by row:
//                                                  # wall, . floor, h a med kit's place) and these players (by sender
//                                                  id, in hex), every second: who comes in the middle can watch
//   arena s <round> <step> <phase> <seconds> <kits> <players> <shots>
//                                                  after each step: the phase (c: counting down, p: playing), the
//                                                  seconds left (to go, or of the round), the med kits (1 there, 0
//                                                  taken, in the order of the map; - for none), the players in the
//                                                  order of the roster, 13 characters each: x (2), y (2), aim (2,
//                                                  degrees clockwise from the right), hits left (1), state (1: 0
//                                                  playing, 1 just back and not to be hit yet, 2 and more out, for
//                                                  that minus 2 seconds), taken out (2), times out (2), by whom they
//                                                  were last taken out (1: place in the roster plus 1, 0 for
//                                                  nobody); then the shots (- for none), 7 characters each: x (2), y
//                                                  (2), direction (2), whose (1). x and y are in 24ths of a tile
//   arena end <round> <place>...                   it is over: where each player finished, 0 for the first (a few
//                                                  times, as everybody rates it)
// and the players send:
//   arena join <round>                             to play, again every second until they are in the open list
//   arena in <round> <move> <aim> <fire> <shots>   where they go (0 to 8: 3 times the vertical direction plus the
//                                                  horizontal one, each 0 back, 1 still, 2 on; 4 stands still), where
//                                                  they aim (degrees), whether they hold the trigger (1) and how many
//                                                  single shots they asked for this round (a terminal's /game arena
//                                                  shoot), when it changes and again every so often
// where <round> is a random hex number naming the round. If the referee goes silent, the round is off, unrated.

#include "game.hpp"

#include "color.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <map>
#include <numbers>
#include <optional>
#include <random>
#include <vector>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;

    constexpr std::string_view game_name = "arena";
    constexpr auto join_time = 20s;
    constexpr std::size_t min_players = 2;
    constexpr std::size_t max_players = 16;

    // The referee's clock: steps of the game.
    constexpr auto step_time = 50ms;
    constexpr int steps_per_second = 20;
    constexpr int countdown_steps = 3 * steps_per_second;
    constexpr int round_steps = 180 * steps_per_second;
    // Once the time is up, the round ends after a moment, for the last shots to be seen.
    constexpr int end_delay_steps = steps_per_second;

    // Positions are in 24ths of a tile; speeds in those per step.
    constexpr int tile = 24;
    constexpr double walk_speed = 5.0;
    // A player is a square of twice this, against the walls, and is hit by a shot this close to their middle.
    constexpr double body = 8.0;
    constexpr double hit_radius = 10.0;
    constexpr double shot_speed = 20.0;
    constexpr int shot_life = 30;
    constexpr int max_shots = 96;
    constexpr int fire_every = 6;
    constexpr int max_hits = 4;
    constexpr int respawn_steps = 3 * steps_per_second;
    constexpr int protect_steps = 2 * steps_per_second;
    constexpr int kit_steps = 15 * steps_per_second;
    constexpr double kit_reach = 14.0;

    // How often things are told again, as packets get lost.
    constexpr auto open_interval = 1s;
    constexpr auto roster_interval = 1s;
    constexpr auto join_interval = 1s;
    constexpr auto input_interval = 300ms;
    constexpr int end_repeats = 5;
    constexpr auto end_interval = 400ms;
    // Nothing from the referee for so long, and the round is off.
    constexpr auto referee_timeout = 6s;

    constexpr int move_still = 4;

    constexpr std::string_view digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_";

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

    // A number in base 64, in so many characters (clamped to what fits).
    void put(std::string& out, int value, int width) {
        const int top = (1 << (6 * width)) - 1;
        value = std::clamp(value, 0, top);
        for (int i = width - 1; i >= 0; --i) {
            out += digits[static_cast<std::size_t>((value >> (6 * i)) & 63)];
        }
    }

    // Reads a number of so many characters from the front of s, and removes them.
    std::optional<int> take(std::string_view& s, int width) {
        if (s.size() < static_cast<std::size_t>(width)) {
            return std::nullopt;
        }
        int value = 0;
        for (int i = 0; i < width; ++i) {
            const auto d = digits.find(s[static_cast<std::size_t>(i)]);
            if (d == std::string_view::npos) {
                return std::nullopt;
            }
            value = value * 64 + static_cast<int>(d);
        }
        s.remove_prefix(static_cast<std::size_t>(width));
        return value;
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

    std::string lower(std::string_view s) {
        std::string out(s);
        for (char& c : out) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return out;
    }

    // The size of the map for so many players: small, so the others are always near; odd, so it has a middle.
    std::pair<int, int> map_size(std::size_t players) {
        if (players <= 4) {
            return {17, 13};
        }
        if (players <= 8) {
            return {21, 15};
        }
        return {25, 17};
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
            {w - 1 - w / 4, b},
            {w - 1 - w / 4, 1},
            {w / 4, b}, // quarters
            {1, h / 4},
            {r, h - 1 - h / 4},
            {r, h / 4},
            {1, h - 1 - h / 4},
        }};
        std::vector<int> out;
        for (const auto& [x, y] : points) {
            out.push_back(y * w + x);
        }
        return out;
    }

    // Whether every floor tile of the map can be walked to from every other.
    bool connected(const std::string& grid, int w) {
        const auto first = grid.find_first_not_of('#');
        if (first == std::string::npos) {
            return false;
        }
        std::vector<bool> seen(grid.size(), false);
        std::vector<int> todo {static_cast<int>(first)};
        seen[first] = true;
        std::size_t reached = 0;
        while (!todo.empty()) {
            const int cell = todo.back();
            todo.pop_back();
            ++reached;
            for (const int d : {1, -1, w, -w}) {
                const auto next = static_cast<std::size_t>(cell + d);
                if (next < grid.size() && !seen[next] && grid[next] != '#') {
                    seen[next] = true;
                    todo.push_back(static_cast<int>(next));
                }
            }
        }
        return reached == static_cast<std::size_t>(std::ranges::count_if(grid, [](char c) {
                   return c != '#';
               }));
    }

    // A map: walls all around, and pieces of cover placed at random in a quarter and mirrored into the others, so
    // that no corner is better than another; the spawns and the middle stay clear, and med kits go in the middle (and,
    // on the bigger maps, in the middles of the top and bottom).
    template <typename Rng>
    std::string make_map(int w, int h, Rng& rng) {
        const auto starts = spawns(w, h);
        std::vector<int> kits {(h / 2) * w + w / 2};
        if (w > 17) {
            kits.push_back(2 * w + w / 2);
            kits.push_back((h - 3) * w + w / 2);
        }
        std::vector<bool> keep_clear(static_cast<std::size_t>(w * h), false);
        for (const int cell : starts) {
            const int sx = cell % w, sy = cell / w;
            for (int y = sy - 1; y <= sy + 1; ++y) {
                for (int x = sx - 1; x <= sx + 1; ++x) {
                    if (x >= 0 && y >= 0 && x < w && y < h) {
                        keep_clear[static_cast<std::size_t>(y * w + x)] = true;
                    }
                }
            }
        }
        for (const int cell : kits) {
            keep_clear[static_cast<std::size_t>(cell)] = true;
        }
        // The pieces, as offsets from where they go.
        const std::array<std::vector<std::pair<int, int>>, 7> pieces {{
            {{0, 0}},
            {{0, 0}, {1, 0}},
            {{0, 0}, {0, 1}},
            {{0, 0}, {1, 0}, {2, 0}},
            {{0, 0}, {0, 1}, {0, 2}},
            {{0, 0}, {1, 0}, {0, 1}},
            {{0, 0}, {1, 0}, {1, 1}},
        }};
        std::string grid;
        for (int attempt = 0; attempt < 60; ++attempt) {
            grid.assign(static_cast<std::size_t>(w * h), '.');
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
                        grid[static_cast<std::size_t>(y * w + x)] = '#';
                    }
                }
            }
            const int qw = w / 2, qh = h / 2; // the top left quarter, with the middle row and column
            const int target = (qw - 1) * (qh - 1) / 6;
            int placed = 0;
            std::uniform_int_distribution<int> px(1, qw), py(1, qh);
            std::uniform_int_distribution<std::size_t> which(0, pieces.size() - 1);
            for (int tries = 0; tries < 200 && placed < target; ++tries) {
                const int x0 = px(rng), y0 = py(rng);
                const auto& piece = pieces[which(rng)];
                bool fits = true;
                for (const auto& [dx, dy] : piece) {
                    const int x = x0 + dx, y = y0 + dy;
                    if (x > qw || y > qh || keep_clear[static_cast<std::size_t>(y * w + x)]) {
                        fits = false;
                    }
                }
                if (!fits) {
                    continue;
                }
                for (const auto& [dx, dy] : piece) {
                    const int x = x0 + dx, y = y0 + dy;
                    for (const auto& [mx, my] : {std::pair {x, y}, std::pair {w - 1 - x, y}, std::pair {x, h - 1 - y},
                                                 std::pair {w - 1 - x, h - 1 - y}}) {
                        grid[static_cast<std::size_t>(my * w + mx)] = '#';
                    }
                }
                placed += static_cast<int>(piece.size());
            }
            if (connected(grid, w)) {
                break;
            }
            // Never stuck: the last try is a plain field of pillars.
            if (attempt == 58) {
                for (std::size_t i = 0; i < grid.size(); ++i) {
                    const int x = static_cast<int>(i) % w, y = static_cast<int>(i) / w;
                    const bool border = x == 0 || y == 0 || x == w - 1 || y == h - 1;
                    grid[i] = border || (x % 4 == 2 && y % 4 == 2 && !keep_clear[i]) ? '#' : '.';
                }
                break;
            }
        }
        for (const int cell : kits) {
            grid[static_cast<std::size_t>(cell)] = 'h';
        }
        return grid;
    }

    class Arena final : public Game {
    public:
        Arena(Chat& chat, Screen& terminal, Games& games) :
            chat_(chat),
            terminal_(terminal),
            games_(games),
            rng_(std::random_device {}()) {
        }

        ~Arena() override {
            chat_.set_fast_ticks(false, fast_ticks_bit);
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "a top-down shooter for everyone: three minutes of deathmatch, the most players taken out wins";
        }

        void start() override {
            if (phase_ == Phase::Lobby) {
                join();
                return;
            }
            if (phase_ == Phase::Match) {
                chat_.game_notice(me_ ? "🎯 You are in a round of arena: /game help arena for the keys."
                                  : terminal_mode_
                                      ? "🎯 A round of arena is on: wait for it to end (/game arena map shows it)."
                                      : "🎯 A round of arena is on: wait for it to end (the window shows it).");
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
                    chat_.game_notice(std::format("🎯 Not enough players joined {}'s round of arena: it is off.",
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
                            chat_.game_notice("🎯 Nobody else joined your round of arena: it is off.");
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
                        chat_.game_notice("🎯 The round of arena is off: its referee is gone.");
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
                    chat_.game_notice("🎯 The round of arena is off: its referee is gone. Nobody wins it.");
                    over(std::string("The referee left: nobody wins."));
                    return;
                }
                if (me_ && now >= next_input_) {
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
            if (word == "in") {
                // From the window: move, aim and trigger.
                const auto move = parse_int(next_field(args));
                const auto aim = parse_int(next_field(args));
                const auto fire = parse_int(next_field(args));
                if (move && aim && fire && *move >= 0 && *move <= 8) {
                    hold(static_cast<int>(*move), static_cast<int>(((*aim % 360) + 360) % 360), *fire != 0);
                }
                return true;
            }
            if (word == "move" || word == "walk") {
                const int move = move_of(args);
                if (move < 0) {
                    chat_.game_notice("🎯 Move up, down, left, right, or between them like upleft (or stop).");
                } else {
                    hold(move, aim_, false);
                }
                return true;
            }
            if (word == "stop") {
                hold(move_still, aim_, false);
                return true;
            }
            if (word == "shoot" || word == "fire") {
                shoot(args);
                return true;
            }
            if (word == "map" || word == "show") {
                if (phase_ == Phase::Match || phase_ == Phase::Over) {
                    draw();
                } else {
                    chat_.game_notice("🎯 No round of arena is being played: /game arena starts one.");
                }
                return true;
            }
            if (word == "score" || word == "scores") {
                if (phase_ == Phase::Match || phase_ == Phase::Over) {
                    scoreboard();
                } else {
                    chat_.game_notice("🎯 No round of arena is being played: /game arena starts one.");
                }
                return true;
            }
            if (const int move = move_of(word); move >= 0 && move != move_still) {
                hold(move, aim_, false);
                return true;
            }
            return false;
        }

        std::vector<Help> help() const override {
            return {
                {"", "start a round: everyone has 20 seconds to join (or join the one that is open)"},
                {"join", "join the round that is open"},
                {"go", "start the round you opened now, without waiting (2 players at least)"},
                {"move up|down|left|right|upleft|...", "keep walking that way, for the terminal; in the window, WASD "
                                                       "or the arrows"},
                {"stop", "stop walking"},
                {"shoot [NAME]", "one shot at NAME, or at whoever is nearest: in the window, aim with the mouse and "
                                 "click (or hold Space)"},
                {"map", "show the map (in the terminal)"},
                {"score", "who took out whom so far"},
            };
        }

    private:
        enum class Phase { None, Lobby, Match, Over };

        // Arena shares the chat's fast ticks with bomber: each of them holds its own.
        static constexpr unsigned fast_ticks_bit = 2;

        // A player, as the referee's last state says.
        struct Seen {
            int x = 0;
            int y = 0;
            int aim = 0;
            int hits = max_hits;
            int state = 0; // 0 playing, 1 protected, 2 + seconds out
            int kills = 0;
            int deaths = 0;
            int killer = -1;
        };
        struct SeenShot {
            int x = 0;
            int y = 0;
            int dir = 0;
            int owner = 0;
        };

        // The referee's own game.
        struct SimPlayer {
            double x = 0;
            double y = 0;
            int aim = 0;
            int move = move_still;
            bool fire = false;
            std::int64_t asked = 0; // single shots asked for
            int pending = 0;        // and not fired yet
            int cooldown = 0;
            int hits = max_hits;
            int out = 0;       // steps until back
            int protect = 0;   // steps not to be hit
            int kills = 0;
            int deaths = 0;
            int killer = -1;
        };
        struct Shot {
            double x = 0;
            double y = 0;
            int dir = 0;
            int owner = 0;
            int life = shot_life;
        };
        struct Sim {
            std::vector<SimPlayer> players;
            std::vector<Shot> shots;
            std::vector<int> spawns;
            std::vector<int> kit_cells;
            std::vector<int> kit_wait; // steps until each med kit is back
            int step = 0;
            int end_at = -1;
            clock::time_point next_step;
            clock::time_point next_roster;
        };

        // 0 to 8 for a direction like "up" or "downleft", -1 for none.
        static int move_of(std::string_view word) {
            std::string w = lower(word);
            std::erase(w, '-');
            if (w == "stop" || w == "none" || w == "n") {
                return move_still;
            }
            int dx = 0, dy = 0;
            std::string_view rest = w;
            bool any = false;
            while (!rest.empty()) {
                if (rest.starts_with("up") || rest.starts_with("north")) {
                    dy = -1;
                    rest.remove_prefix(rest.starts_with("up") ? 2 : 5);
                } else if (rest.starts_with("down") || rest.starts_with("south")) {
                    dy = 1;
                    rest.remove_prefix(rest.starts_with("down") ? 4 : 5);
                } else if (rest.starts_with("left") || rest.starts_with("west")) {
                    dx = -1;
                    rest.remove_prefix(4);
                } else if (rest.starts_with("right") || rest.starts_with("east")) {
                    dx = 1;
                    rest.remove_prefix(rest.starts_with("right") ? 5 : 4);
                } else if (rest.size() <= 2 && rest.find_first_not_of("udlr") == std::string_view::npos) {
                    for (const char c : rest) {
                        dx = c == 'l' ? -1 : c == 'r' ? 1 : dx;
                        dy = c == 'u' ? -1 : c == 'd' ? 1 : dy;
                    }
                    rest = {};
                } else {
                    return -1;
                }
                any = true;
            }
            return any ? (dy + 1) * 3 + (dx + 1) : -1;
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
            shots_.clear();
            me_.reset();
            result_.clear();
            places_.clear();
            frame_ = -1;
            last_heard_ = clock::now();
            next_join_ = {};
            chat_.game_notice(
                std::format("🎯 Arena, started by {}! /game arena join within {} seconds to play: three minutes of "
                            "shooting each other, whoever takes out the most wins (/game help arena).",
                            colored(referee), join_time.count()));
        }

        void join() {
            if (phase_ != Phase::Lobby) {
                chat_.game_notice("🎯 No round of arena is open: /game arena starts one.");
                return;
            }
            if (joined_) {
                chat_.game_notice("🎯 You are in: the round starts when the time to join is up.");
                return;
            }
            joined_ = true;
            if (referee_ == chat_.id()) {
                return;
            }
            send(std::format("join {:x}", round_));
            next_join_ = clock::now() + join_interval;
            chat_.game_notice("🎯 Joining the round of arena…");
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
                    chat_.game_notice(std::format("🎯 {} joins the round of arena.", colored(id)));
                }
            }
            lobby_ = std::move(ids);
            lobby_left_ = static_cast<int>(*seconds);
            if (joined_ && std::ranges::find(lobby_, chat_.id()) != lobby_.end() && !said_in_) {
                said_in_ = true;
                chat_.game_notice(std::format("🎯 You are in! The round starts in {}.", plural(*seconds, "second")));
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
            chat_.game_notice(std::format("🎯 {} joins the round of arena.", chat_.colored_name(sender, name)));
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
                chat_.game_notice(phase_ == Phase::Lobby ? "🎯 Only who opened the round can start it early."
                                                         : "🎯 No round of arena is open: /game arena opens one.");
                return;
            }
            if (lobby_.size() < min_players) {
                chat_.game_notice("🎯 It takes 2 players at least: wait for somebody to join.");
                return;
            }
            begin_match();
        }

        // --- The match: the referee's side.

        void begin_match() {
            const auto [w, h] = map_size(lobby_.size());
            Sim sim;
            const std::string grid = make_map(w, h, rng_);
            sim.spawns = spawns(w, h);
            for (std::size_t i = 0; i < grid.size(); ++i) {
                if (grid[i] == 'h') {
                    sim.kit_cells.push_back(static_cast<int>(i));
                }
            }
            sim.kit_wait.assign(sim.kit_cells.size(), 0);
            // Players get the spawns in a random order, aiming at the middle.
            auto starts = sim.spawns;
            starts.resize(lobby_.size());
            std::ranges::shuffle(starts, rng_);
            for (const int cell : starts) {
                SimPlayer p;
                p.x = (cell % w) * tile + tile / 2.0;
                p.y = (cell / w) * tile + tile / 2.0;
                p.aim = angle_to(p.x, p.y, w * tile / 2.0, h * tile / 2.0);
                sim.players.push_back(p);
            }
            sim.next_step = clock::now();
            sim.next_roster = clock::now();
            roster_ = lobby_;
            begin_view(w, h, grid);
            sim_ = std::move(sim);
            chat_.set_fast_ticks(true, fast_ticks_bit);
            send_roster();
            chat_.game_notice(std::format("🎯 The round of arena starts, with {}!", roster_names()));
        }

        static int angle_to(double x, double y, double tx, double ty) {
            const double a = std::atan2(ty - y, tx - x) * 180.0 / std::numbers::pi;
            return (static_cast<int>(std::lround(a)) % 360 + 360) % 360;
        }

        void send_roster() {
            std::string ids;
            for (const std::uint64_t id : roster_) {
                ids += std::format(" {:x}", id);
            }
            send(std::format("roster {:x} {} {} {}{}", round_, w_, h_, map_, ids));
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
            // The referee sees what it sends, as everybody: skipping "<round> ".
            std::string_view text = state;
            next_field(text);
            apply_state(text);
            if (s.end_at >= 0 && s.step >= s.end_at) {
                places_ = placings(s.players);
                end_sent_ = 0;
                send_end();
                finish(places_);
            }
        }

        // Whoever took out more is better placed; between those even, whoever was out fewer times.
        template <typename Player>
        static std::vector<int> placings(const std::vector<Player>& players) {
            std::vector<int> places;
            const auto better = [](const Player& a, const Player& b) {
                return a.kills > b.kills || (a.kills == b.kills && a.deaths < b.deaths);
            };
            for (const Player& p : players) {
                places.push_back(static_cast<int>(std::ranges::count_if(players, [&](const Player& o) {
                    return better(o, p);
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
            const auto move = parse_int(next_field(text));
            const auto aim = parse_int(next_field(text));
            const auto fire = parse_int(next_field(text));
            const auto asked = parse_int(next_field(text));
            if (!move || !aim || !fire || !asked || *move < 0 || *move > 8 || *aim < 0 || *aim >= 360) {
                return;
            }
            control(static_cast<std::size_t>(at - roster_.begin()), static_cast<int>(*move), static_cast<int>(*aim),
                    *fire != 0, *asked);
        }

        // A player's controls, from their packet or (ours) right away.
        void control(std::size_t index, int move, int aim, bool fire, std::int64_t asked) {
            SimPlayer& p = sim_->players[index];
            p.move = move;
            p.aim = aim;
            p.fire = fire;
            if (asked > p.asked) {
                p.pending = std::min<int>(3, p.pending + static_cast<int>(asked - p.asked));
                p.asked = asked;
            }
        }

        bool wall(double x, double y) const {
            const int cx = static_cast<int>(std::floor(x / tile)), cy = static_cast<int>(std::floor(y / tile));
            return cx < 0 || cy < 0 || cx >= w_ || cy >= h_ || map_[static_cast<std::size_t>(cy * w_ + cx)] == '#';
        }

        // Whether a player's square there would be in a wall.
        bool blocked(double x, double y) const {
            return wall(x - body, y - body) || wall(x + body - 0.01, y - body) || wall(x - body, y + body - 0.01) ||
                   wall(x + body - 0.01, y + body - 0.01);
        }

        // Walks a player along where they go, sliding along the walls, a few units at a time.
        void walk(SimPlayer& p) const {
            const int dx = p.move % 3 - 1, dy = p.move / 3 - 1;
            if (dx == 0 && dy == 0) {
                return;
            }
            const double length = std::sqrt(static_cast<double>(dx * dx + dy * dy));
            const double vx = dx / length * walk_speed, vy = dy / length * walk_speed;
            constexpr int parts = 2;
            for (int i = 0; i < parts; ++i) {
                if (!blocked(p.x + vx / parts, p.y)) {
                    p.x += vx / parts;
                }
                if (!blocked(p.x, p.y + vy / parts)) {
                    p.y += vy / parts;
                }
            }
        }

        // Back in: at the spawn farthest from everybody playing.
        void respawn(Sim& s, std::size_t index) {
            int best = s.spawns.front();
            double best_distance = -1;
            for (const int cell : s.spawns) {
                const double x = (cell % w_) * tile + tile / 2.0, y = (cell / w_) * tile + tile / 2.0;
                double nearest = 1e9;
                for (std::size_t i = 0; i < s.players.size(); ++i) {
                    const SimPlayer& o = s.players[i];
                    if (i != index && o.out == 0) {
                        nearest = std::min(nearest, std::hypot(o.x - x, o.y - y));
                    }
                }
                if (nearest > best_distance) {
                    best_distance = nearest;
                    best = cell;
                }
            }
            SimPlayer& p = s.players[index];
            p.x = (best % w_) * tile + tile / 2.0;
            p.y = (best / w_) * tile + tile / 2.0;
            p.hits = max_hits;
            p.protect = protect_steps;
            p.cooldown = 0;
            p.pending = 0;
        }

        void step(Sim& s) {
            ++s.step;
            const bool playing = s.step > countdown_steps && s.step <= countdown_steps + round_steps;
            for (std::size_t i = 0; i < s.players.size(); ++i) {
                SimPlayer& p = s.players[i];
                if (p.out > 0) {
                    if (--p.out == 0) {
                        respawn(s, i);
                    }
                    continue;
                }
                p.protect = std::max(0, p.protect - 1);
                p.cooldown = std::max(0, p.cooldown - 1);
                if (!playing) {
                    continue;
                }
                walk(p);
                if ((p.fire || p.pending > 0) && p.cooldown == 0 && static_cast<int>(s.shots.size()) < max_shots) {
                    const double a = p.aim * std::numbers::pi / 180.0;
                    s.shots.push_back({p.x + std::cos(a) * body, p.y + std::sin(a) * body, p.aim, static_cast<int>(i)});
                    p.cooldown = fire_every;
                    p.protect = 0;
                    if (!p.fire && p.pending > 0) {
                        --p.pending;
                    }
                }
                // A med kit heals whoever is hurt and walks over it.
                for (std::size_t k = 0; k < s.kit_cells.size(); ++k) {
                    const int cell = s.kit_cells[k];
                    const double kx = (cell % w_) * tile + tile / 2.0, ky = (cell / w_) * tile + tile / 2.0;
                    if (s.kit_wait[k] == 0 && p.hits < max_hits && std::hypot(p.x - kx, p.y - ky) < kit_reach) {
                        p.hits = max_hits;
                        s.kit_wait[k] = kit_steps;
                    }
                }
            }
            for (int& wait : s.kit_wait) {
                wait = std::max(0, wait - 1);
            }
            // The shots fly a few units at a time, so they hit what is in the way.
            constexpr int parts = 5;
            for (Shot& shot : s.shots) {
                const double a = shot.dir * std::numbers::pi / 180.0;
                const double vx = std::cos(a) * shot_speed / parts, vy = std::sin(a) * shot_speed / parts;
                for (int part = 0; part < parts && shot.life > 0; ++part) {
                    shot.x += vx;
                    shot.y += vy;
                    if (wall(shot.x, shot.y)) {
                        shot.life = 0;
                        break;
                    }
                    for (std::size_t i = 0; i < s.players.size(); ++i) {
                        SimPlayer& p = s.players[i];
                        if (static_cast<int>(i) == shot.owner || p.out > 0 ||
                            std::hypot(p.x - shot.x, p.y - shot.y) > hit_radius) {
                            continue;
                        }
                        shot.life = 0;
                        if (p.protect > 0 || !playing) {
                            break;
                        }
                        if (--p.hits <= 0) {
                            p.out = respawn_steps;
                            p.fire = false;
                            ++p.deaths;
                            p.killer = shot.owner;
                            ++s.players[static_cast<std::size_t>(shot.owner)].kills;
                        }
                        break;
                    }
                }
                --shot.life;
            }
            std::erase_if(s.shots, [](const Shot& shot) {
                return shot.life <= 0;
            });
            if (s.step >= countdown_steps + round_steps && s.end_at < 0) {
                s.end_at = s.step + end_delay_steps;
            }
        }

        std::string encode_state(const Sim& s) const {
            char phase = 'p';
            int seconds = 0;
            if (s.step <= countdown_steps) {
                phase = 'c';
                seconds = (countdown_steps - s.step + steps_per_second - 1) / steps_per_second;
            } else {
                seconds =
                    std::max(0, (countdown_steps + round_steps - s.step + steps_per_second - 1) / steps_per_second);
            }
            std::string kits;
            for (const int wait : s.kit_wait) {
                kits += wait == 0 ? '1' : '0';
            }
            std::string players;
            for (const SimPlayer& p : s.players) {
                put(players, static_cast<int>(std::lround(p.x)), 2);
                put(players, static_cast<int>(std::lround(p.y)), 2);
                put(players, p.aim, 2);
                put(players, p.hits, 1);
                put(players, p.out > 0 ? 2 + (p.out + steps_per_second - 1) / steps_per_second : p.protect > 0 ? 1 : 0,
                    1);
                put(players, p.kills, 2);
                put(players, p.deaths, 2);
                put(players, p.killer + 1, 1);
            }
            std::string shots;
            for (const Shot& shot : s.shots) {
                put(shots, static_cast<int>(std::lround(std::max(0.0, shot.x))), 2);
                put(shots, static_cast<int>(std::lround(std::max(0.0, shot.y))), 2);
                put(shots, shot.dir, 2);
                put(shots, shot.owner, 1);
            }
            return std::format("{:x} {} {} {} {} {} {}", round_, s.step, phase, seconds, kits.empty() ? "-" : kits,
                               players, shots.empty() ? "-" : shots);
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
            const std::string_view map = next_field(text);
            if (!w || !h || *w < 5 || *h < 5 || *w > 31 || *h > 31 ||
                map.size() != static_cast<std::size_t>(*w * *h) ||
                map.find_first_not_of("#.h") != std::string_view::npos) {
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
            begin_view(static_cast<int>(*w), static_cast<int>(*h), std::string(map));
            chat_.set_fast_ticks(true, fast_ticks_bit);
            last_heard_ = clock::now();
            if (me_) {
                chat_.game_notice(std::format("🎯 The round of arena starts, with {}! {}", roster_names(),
                                              terminal_mode_ ? "/game arena move up (down, left, right, upleft…) "
                                                               "walks, /game arena shoot [NAME] shoots, /game arena "
                                                               "map shows it."
                                                             : "WASD moves, the mouse aims and shoots."));
                terminal_.bell();
            } else if (was_lobby) {
                chat_.game_notice(std::format("🎯 The round of arena starts, with {}: {} shows it.", roster_names(),
                                              terminal_mode_ ? "/game arena map" : "the window"));
            }
        }

        // A new match, with roster_: everybody is where the first state puts them.
        void begin_view(int w, int h, std::string map) {
            phase_ = Phase::Match;
            w_ = w;
            h_ = h;
            map_ = std::move(map);
            kits_.assign(static_cast<std::size_t>(std::ranges::count(map_, 'h')), true);
            seen_.assign(roster_.size(), Seen {});
            shots_.clear();
            me_.reset();
            const auto at = std::ranges::find(roster_, chat_.id());
            if (at != roster_.end()) {
                me_ = static_cast<std::size_t>(at - roster_.begin());
            }
            move_ = move_still;
            aim_ = 0;
            fire_ = false;
            asked_ = 0;
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
            const std::string_view kits = next_field(text);
            std::string_view players = next_field(text);
            std::string_view shots = next_field(text);
            if (!frame || *frame <= frame_ || phase.size() != 1 || !seconds ||
                (kits != "-" && (kits.size() != kits_.size() || kits.find_first_not_of("01") != std::string_view::npos)) ||
                players.size() != roster_.size() * 13) {
                return;
            }
            const int limit = std::max(w_, h_) * tile;
            std::vector<Seen> seen;
            while (!players.empty()) {
                Seen p;
                const auto x = take(players, 2), y = take(players, 2), aim = take(players, 2), hits = take(players, 1),
                           state = take(players, 1), kills = take(players, 2), deaths = take(players, 2),
                           killer = take(players, 1);
                if (!x || !y || !aim || !hits || !state || !kills || !deaths || !killer || *x >= limit ||
                    *y >= limit) {
                    return;
                }
                p.x = *x;
                p.y = *y;
                p.aim = *aim % 360;
                p.hits = std::min(*hits, max_hits);
                p.state = *state;
                p.kills = *kills;
                p.deaths = *deaths;
                p.killer = *killer > static_cast<int>(roster_.size()) ? -1 : *killer - 1;
                seen.push_back(p);
            }
            std::vector<SeenShot> flying;
            if (shots != "-") {
                if (shots.size() % 7 != 0) {
                    return;
                }
                while (!shots.empty()) {
                    const auto x = take(shots, 2), y = take(shots, 2), dir = take(shots, 2), owner = take(shots, 1);
                    if (!x || !y || !dir || !owner) {
                        return;
                    }
                    flying.push_back({*x, *y, *dir % 360, *owner});
                }
            }
            frame_ = *frame;
            const char was = sub_;
            sub_ = phase[0];
            seconds_ = static_cast<int>(*seconds);
            for (std::size_t i = 0; i < kits_.size() && kits != "-"; ++i) {
                kits_[i] = kits[i] == '1';
            }
            // Who was taken out since the last state: the window shows it, a terminal tells.
            for (std::size_t i = 0; i < seen.size(); ++i) {
                if (seen_[i].state < 2 && seen[i].state >= 2 && frame_ > 0) {
                    obituary(i, seen[i].killer);
                }
            }
            seen_ = std::move(seen);
            shots_ = std::move(flying);
            if (was == 'c' && sub_ == 'p') {
                chat_.game_notice("🎯 Go!");
                if (terminal_mode_ && me_) {
                    draw();
                }
            }
            publish();
        }

        void obituary(std::size_t victim, int killer) {
            if (!terminal_mode_) {
                return;
            }
            const std::string who = colored(roster_[victim]);
            if (killer >= 0 && static_cast<std::size_t>(killer) < roster_.size()) {
                chat_.game_notice(
                    std::format("🎯 {} took out {}.", colored(roster_[static_cast<std::size_t>(killer)]), who));
            }
            if (me_ && *me_ == victim) {
                terminal_.bell();
                chat_.game_notice(std::format("   You are out: back in {} seconds.", respawn_steps / steps_per_second));
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
                result = "Everybody did as well: a draw!";
                chat_.game_notice("🎯 Arena is over: everybody did as well, a draw!");
            } else {
                int kills = 0;
                for (std::size_t i = 0; i < roster_.size(); ++i) {
                    if (places[i] == 0) {
                        games_.add_win(game_name, roster_[i], name_of(roster_[i]));
                        kills = i < seen_.size() ? seen_[i].kills : 0;
                    }
                }
                result = std::format("{} win{}!", plain_winners, first == 1 ? "s" : "");
                chat_.game_notice(std::format("🏆 {} win{} arena, with {} taken out!", winners, first == 1 ? "s" : "",
                                              kills));
            }
            if (terminal_mode_) {
                scoreboard();
            }
            games_.rate(game_name, placings);
            over(std::move(result));
        }

        void over(std::string result) {
            phase_ = Phase::Over;
            sim_.reset();
            result_ = std::move(result);
            fire_ = false;
            chat_.set_fast_ticks(false, fast_ticks_bit);
            publish();
        }

        // --- Our controls.

        void hold(int move, int aim, bool fire) {
            if (!me_ || phase_ != Phase::Match) {
                return;
            }
            const bool changed = move != move_ || aim != aim_ || fire != fire_;
            move_ = move;
            aim_ = aim;
            fire_ = fire;
            if (changed) {
                send_input();
            }
        }

        // One shot from the terminal: at NAME, or at the nearest one playing.
        void shoot(std::string_view who) {
            if (!me_ || phase_ != Phase::Match) {
                chat_.game_notice("🎯 You are not playing a round of arena: /game arena starts one.");
                return;
            }
            const Seen& me = seen_[*me_];
            if (me.state >= 2) {
                chat_.game_notice("🎯 You are out: wait to be back.");
                return;
            }
            std::string name = lower(who);
            if (name.starts_with('@')) {
                name.erase(0, 1);
            }
            std::optional<std::size_t> target;
            double nearest = 1e9;
            for (std::size_t i = 0; i < seen_.size(); ++i) {
                if (i == *me_ || seen_[i].state >= 2) {
                    continue;
                }
                if (!name.empty() && lower(name_of(roster_[i])) != name) {
                    continue;
                }
                const double d = std::hypot(seen_[i].x - me.x, seen_[i].y - me.y);
                if (d < nearest) {
                    nearest = d;
                    target = i;
                }
            }
            if (!target) {
                chat_.game_notice(name.empty() ? "🎯 Nobody to shoot at." : "🎯 Nobody by that name is in to shoot at.");
                return;
            }
            aim_ = angle_to(me.x, me.y, seen_[*target].x, seen_[*target].y);
            ++asked_;
            send_input();
        }

        void send_input() {
            next_input_ = clock::now() + input_interval;
            if (sim_) {
                control(*me_, move_, aim_, fire_, asked_);
                return;
            }
            send(std::format("in {:x} {} {} {} {}", round_, move_, aim_, fire_ ? 1 : 0, asked_));
        }

        // --- Showing it.

        // The map in the terminal, two characters per tile, with the players by letter.
        void draw() const {
            if (seen_.empty()) {
                return;
            }
            const bool vt = terminal_.colors();
            std::vector<int> who(map_.size(), -1);
            for (std::size_t i = 0; i < seen_.size(); ++i) {
                if (seen_[i].state < 2) {
                    who[static_cast<std::size_t>((seen_[i].y / tile) * w_ + seen_[i].x / tile)] = static_cast<int>(i);
                }
            }
            std::vector<bool> shot(map_.size(), false);
            for (const SeenShot& s : shots_) {
                const int cx = s.x / tile, cy = s.y / tile;
                if (cx >= 0 && cy >= 0 && cx < w_ && cy < h_) {
                    shot[static_cast<std::size_t>(cy * w_ + cx)] = true;
                }
            }
            std::vector<bool> kit(map_.size(), false);
            for (std::size_t i = 0, k = 0; i < map_.size(); ++i) {
                if (map_[i] == 'h') {
                    kit[i] = k < kits_.size() && kits_[k];
                    ++k;
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
                    const bool wall = map_[cell] == '#';
                    const std::string_view mark = wall ? (vt ? "  " : "##") : shot[cell] ? "··" : kit[cell] ? "++" : "  ";
                    if (!vt) {
                        out += mark;
                        continue;
                    }
                    if (wall) {
                        out += bg(96, 102, 116) + "  ";
                    } else if (shot[cell]) {
                        out += bg(40, 44, 52) + "\x1b[38;2;255;220;90;1m··";
                    } else if (kit[cell]) {
                        out += bg(40, 44, 52) + "\x1b[38;2;90;220;120;1m++";
                    } else {
                        out += bg(40, 44, 52) + "  ";
                    }
                    out += "\x1b[0m";
                }
            }
            chat_.game_notice(sub_ == 'c' ? std::format("🎯 Arena starts in {}:", plural(seconds_, "second"))
                                          : std::format("🎯 Arena, {} left:", plural(seconds_, "second")));
            chat_.game_print(out);
            for (std::size_t i = 0; i < seen_.size(); ++i) {
                const Seen& p = seen_[i];
                chat_.game_notice(std::format("   {} {}{}: {}", static_cast<char>('A' + i), colored(roster_[i]),
                                              me_ && *me_ == i ? " (you, @)" : "",
                                              p.state >= 2 ? std::format("out, back in {}s", p.state - 2)
                                                           : std::format("{} of {} hits left", p.hits, max_hits)));
            }
            chat_.game_notice(std::format("   {} wall, ·· shots, ++ med kit; @ is you", vt ? "grey" : "##"));
        }

        void scoreboard() const {
            std::vector<std::size_t> order(seen_.size());
            for (std::size_t i = 0; i < order.size(); ++i) {
                order[i] = i;
            }
            std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
                return seen_[a].kills > seen_[b].kills ||
                       (seen_[a].kills == seen_[b].kills && seen_[a].deaths < seen_[b].deaths);
            });
            chat_.game_notice("🎯 Arena, who took out whom:");
            for (const std::size_t i : order) {
                chat_.game_notice(std::format("   {}{}: {} taken out, out {}", colored(roster_[i]),
                                              me_ && *me_ == i ? " (you)" : "", seen_[i].kills,
                                              plural(seen_[i].deaths, "time")));
            }
        }

        // The round, for a window of its own (see Screen::show_game()).
        void publish() {
            const char* phase = phase_ == Phase::None    ? "none"
                                : phase_ == Phase::Lobby ? "lobby"
                                : phase_ == Phase::Over  ? "over"
                                : sub_ == 'c'            ? "countdown"
                                                         : "play";
            std::string players = "[";
            const auto& ids = phase_ == Phase::Lobby ? lobby_ : roster_;
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const Color c = color_of_id(ids[i]);
                const Seen p = i < seen_.size() ? seen_[i] : Seen {};
                players += std::format(
                    "{}{{\"name\":{},\"color\":\"#{:02x}{:02x}{:02x}\",\"you\":{},\"x\":{},\"y\":{},\"aim\":{},"
                    "\"hits\":{},\"state\":{},\"kills\":{},\"deaths\":{},\"killer\":{},\"place\":{}}}",
                    i == 0 ? "" : ",", json(name_of(ids[i])), c.r, c.g, c.b, ids[i] == chat_.id(), p.x, p.y, p.aim,
                    p.hits, p.state, p.kills, p.deaths, p.killer, i < places_.size() ? places_[i] : -1);
            }
            players += "]";
            std::string shots = "[";
            for (std::size_t i = 0; i < shots_.size(); ++i) {
                const SeenShot& s = shots_[i];
                shots += std::format("{}[{},{},{},{}]", i == 0 ? "" : ",", s.x, s.y, s.dir, s.owner);
            }
            shots += "]";
            std::string kits = "[";
            for (std::size_t i = 0; i < kits_.size(); ++i) {
                kits += std::format("{}{}", i == 0 ? "" : ",", kits_[i] ? "true" : "false");
            }
            kits += "]";
            const std::string state = std::format(
                "{{\"round\":\"{:x}\",\"phase\":\"{}\",\"referee\":{},\"isReferee\":{},\"joined\":{},\"playing\":{},"
                "\"joinLeft\":{},\"left\":{},\"frame\":{},\"w\":{},\"h\":{},\"unit\":{},\"grid\":{},\"maxHits\":{},"
                "\"shotSpeed\":{},\"body\":{},\"players\":{},\"shots\":{},\"kits\":{},\"result\":{}}}",
                round_, phase, json(phase_ == Phase::None ? std::string() : name_of(referee_)), referee_ == chat_.id(),
                joined_, me_.has_value(), lobby_left_, seconds_, frame_, w_, h_, tile, json(map_), max_hits,
                shot_speed, body, players, shots, kits, json(result_));
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

        // The match, as the roster and the referee's last state say.
        std::vector<std::uint64_t> roster_;
        int w_ = 0;
        int h_ = 0;
        std::string map_;
        std::vector<bool> kits_;
        std::vector<Seen> seen_;
        std::vector<SeenShot> shots_;
        std::int64_t frame_ = -1;
        char sub_ = 'c';
        int seconds_ = 0;
        // Us among the players, by place in the roster.
        std::optional<std::size_t> me_;
        // Our controls: where we go, aim, whether we hold the trigger, the single shots asked for.
        int move_ = move_still;
        int aim_ = 0;
        bool fire_ = false;
        std::int64_t asked_ = 0;
        clock::time_point next_input_;

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

std::unique_ptr<Game> make_arena(Chat& chat, Screen& terminal, Games& games) {
    return std::make_unique<Arena>(chat, terminal, games);
}

} // namespace zchat::game
