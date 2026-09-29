// Dice: push your luck, with /game dice roll and /game dice stop. Everyone rolls a die as many times as they like, each
// roll adding to their points, which only they see; a 1 loses them all. Stopping keeps them, and shows them to
// everybody. The highest points kept when time is up win; whoever has not stopped by then loses theirs.
//
// Everyone rolls their own die, so there is no referee after the start. The one who starts a round sends, as a
// Game packet:
//   dice start <round>                  a round starts: there is some time to join it, then some more to play
// and every player sends:
//   dice play <round>                   joins, with the first roll
//   dice stop <round> <points> <rolls>  keeps the points
//   dice bust <round> <rolls>           rolled a 1
// where <round> is a random hex number naming the round. Everybody ends the round on their own clock, when time is
// up or when all the players are done (and nobody can join anymore), and shows who won from what they heard.

#include "game.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <optional>
#include <random>
#include <vector>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;

    constexpr std::string_view game_name = "dice";
    // Rolling for the first time joins a round, until then.
    constexpr auto join_time = 15s;
    constexpr auto round_time = 60s;
    // The players still rolling are told when so much time is left.
    constexpr auto warning_time = 10s;

    std::optional<std::uint64_t> parse_number(std::string_view s, int base = 10) {
        std::uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
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

    std::string plural(std::uint64_t n, std::string_view word) {
        return std::format("{} {}{}", n, word, n == 1 ? "" : "s");
    }

    class Dice final : public Game {
    public:
        Dice(Chat& chat, Terminal& terminal, Games& games) :
            chat_(chat),
            terminal_(terminal),
            games_(games),
            rng_(std::random_device {}()) {
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "push your luck: roll to add points only you see, but a 1 loses them; the highest kept wins";
        }

        void start() override {
            if (on_) {
                chat_.notice(joinable() ? "A round of dice is on: /game dice roll to join it!"
                                        : "A round of dice is on: wait for it to end.");
                return;
            }
            std::uint64_t round = 0;
            do {
                round = rng_();
            } while (round == 0);
            begin(round, chat_.colored_own_name());
            send(std::format("start {:x}", round_));
        }

        void receive(std::uint64_t sender, std::string_view name, std::string_view text) override {
            const std::string_view event = next_field(text);
            const auto round = parse_number(next_field(text), 16);
            if (!round || *round == 0) {
                return;
            }
            if (event == "start") {
                // Two rounds started at the same time: everybody plays the one with the lowest number, if nobody
                // has rolled yet.
                if (!on_ || (*round < round_ && players_.empty())) {
                    begin(*round, chat_.colored_name(sender, name));
                    terminal_.bell();
                }
                return;
            }
            if (!on_ || *round != round_) {
                return;
            }
            if (event == "play") {
                player(sender, name);
                chat_.notice(std::format("🎲 {} is rolling…", chat_.colored_name(sender, name)));
            } else if (event == "stop") {
                const auto points = parse_number(next_field(text));
                const auto rolls = parse_number(next_field(text));
                if (points && rolls) {
                    stopped(player(sender, name), *points, *rolls);
                }
            } else if (event == "bust") {
                if (const auto rolls = parse_number(next_field(text))) {
                    bust(player(sender, name), *rolls);
                }
            }
        }

        void message(std::uint64_t, std::string_view, std::string_view) override {
        }

        void tick() override {
            if (!on_) {
                return;
            }
            const auto now = clock::now();
            if (me_ && own().status == Status::Rolling && !warned_ && now >= started_ + round_time - warning_time) {
                warned_ = true;
                chat_.notice(std::format("⏳ {} seconds left: /game dice stop to keep your {}, or lose them!",
                                         warning_time.count(), plural(own().points, "point")));
                terminal_.bell();
            }
            const bool all_done = std::ranges::none_of(players_, [](const Player& p) {
                return p.status == Status::Rolling;
            });
            if (now >= started_ + round_time || (!joinable() && all_done)) {
                end();
            }
        }

        bool command(std::string_view args) override {
            if (args == "roll") {
                roll();
                return true;
            }
            if (args == "stop") {
                stop();
                return true;
            }
            return false;
        }

        std::string_view commands() const override {
            return "roll, stop";
        }

    private:
        enum class Status { Rolling, Stopped, Bust };

        struct Player {
            std::uint64_t id = 0;
            std::string name;
            std::string colored_name;
            Status status = Status::Rolling;
            std::uint64_t points = 0;
            std::uint64_t rolls = 0;
        };

        void begin(std::uint64_t round, std::string referee_name) {
            on_ = true;
            round_ = round;
            started_ = clock::now();
            players_.clear();
            me_.reset();
            warned_ = false;
            chat_.notice(std::format("🎲 Dice, started by {}! /game dice roll rolls a die: each roll adds to your "
                                     "points, which only you see, but a 1 loses them all. /game dice stop keeps them, "
                                     "and shows them to everyone.",
                                     referee_name));
            chat_.notice(std::format("   The highest points kept in {} seconds win; whoever has not stopped by then "
                                     "loses theirs. Roll within {} seconds to join.",
                                     round_time.count(), join_time.count()));
        }

        bool joinable() const {
            return on_ && clock::now() < started_ + join_time;
        }

        // The player, added if we had not heard of them (their "play" got lost, or it is us).
        Player& player(std::uint64_t id, std::string_view name) {
            auto it = std::ranges::find(players_, id, &Player::id);
            if (it == players_.end()) {
                players_.push_back({id, std::string(name), chat_.colored_name(id, name)});
                it = players_.end() - 1;
            }
            return *it;
        }

        void roll() {
            if (!on_) {
                chat_.notice("No round of dice is on: /game dice starts one.");
                return;
            }
            if (!me_) {
                if (!joinable()) {
                    chat_.notice("Too late to join this round of dice: wait for the next one.");
                    return;
                }
                me_ = chat_.id();
                player(*me_, chat_.name());
                send(std::format("play {:x}", round_));
            }
            Player& me = own();
            if (me.status != Status::Rolling) {
                chat_.notice("You are done for this round of dice.");
                return;
            }
            const auto face = std::uniform_int_distribution<std::uint64_t>(1, 6)(rng_);
            if (face == 1) {
                chat_.notice(me.points == 0 ? std::string("🎲 1! You are out of this round.")
                                            : std::format("🎲 1! You lose your {}.", plural(me.points, "point")));
                send(std::format("bust {:x} {}", round_, me.rolls + 1));
                bust(me, me.rolls + 1);
                return;
            }
            ++me.rolls;
            me.points += face;
            chat_.notice(
                std::format("🎲 {}: you have {} (only you see them). /game dice roll again, or /game dice stop "
                            "to keep them.",
                            face, plural(me.points, "point")));
        }

        void stop() {
            if (!on_ || !me_) {
                chat_.notice(on_ ? "You are not playing this round of dice: /game dice roll to join it."
                                 : "No round of dice is on: /game dice starts one.");
                return;
            }
            Player& me = own();
            if (me.status != Status::Rolling) {
                chat_.notice("You are done for this round of dice.");
                return;
            }
            send(std::format("stop {:x} {} {}", round_, me.points, me.rolls));
            stopped(me, me.points, me.rolls);
        }

        // Us among the players, once we play.
        Player& own() {
            return player(*me_, chat_.name());
        }

        void stopped(Player& p, std::uint64_t points, std::uint64_t rolls) {
            if (p.status != Status::Rolling) {
                return;
            }
            p.status = Status::Stopped;
            p.points = points;
            p.rolls = rolls;
            chat_.notice(std::format("✋ {} stops with {} ({})", p.colored_name, plural(points, "point"),
                                     plural(rolls, "roll")));
        }

        void bust(Player& p, std::uint64_t rolls) {
            if (p.status != Status::Rolling) {
                return;
            }
            p.status = Status::Bust;
            p.points = 0;
            p.rolls = rolls;
            chat_.notice(rolls <= 1 ? std::format("💥 {} rolled a 1 on the first roll and is out!", p.colored_name)
                                    : std::format("💥 {} rolled a 1 after {} and is out!", p.colored_name,
                                                  plural(rolls - 1, "good roll")));
        }

        void end() {
            on_ = false;
            if (players_.empty()) {
                chat_.notice("🎲 Nobody played that round of dice.");
                return;
            }
            std::string late;
            for (Player& p : players_) {
                if (p.status == Status::Rolling) {
                    p.status = Status::Bust;
                    late += std::format("{}{}", late.empty() ? "" : ", ", p.colored_name);
                }
            }
            if (!late.empty()) {
                chat_.notice(std::format("⏰ Time's up! Not stopped in time, and out: {}", late));
            }

            std::vector<const Player*> ranking;
            for (const Player& p : players_) {
                if (p.status == Status::Stopped) {
                    ranking.push_back(&p);
                }
            }
            std::ranges::stable_sort(ranking, std::ranges::greater {}, &Player::points);
            if (ranking.empty() || ranking.front()->points == 0) {
                chat_.notice("🎲 Dice is over: nobody kept any points.");
                return;
            }
            const std::uint64_t best = ranking.front()->points;
            std::string winners;
            std::string others;
            int tied = 0;
            for (const Player* p : ranking) {
                if (p->points == best) {
                    winners += std::format("{}{}", winners.empty() ? "" : " and ", p->colored_name);
                    ++tied;
                    games_.add_win(game_name, p->id, p->name);
                } else {
                    others += std::format("{}{} {}", others.empty() ? "" : ", ", p->colored_name, p->points);
                }
            }
            chat_.notice(std::format("🏆 {} win{} dice with {}!{}", winners, tied == 1 ? "s" : "",
                                     plural(best, "point"), others.empty() ? "" : std::format(" Then: {}", others)));
        }

        void send(std::string_view event) {
            chat_.send_game(std::format("{} {}", game_name, event));
        }

        Chat& chat_;
        Terminal& terminal_;
        Games& games_;
        std::mt19937_64 rng_;

        bool on_ = false;
        std::uint64_t round_ = 0;
        clock::time_point started_;
        std::vector<Player> players_;
        // Our id among the players, once we play the round.
        std::optional<std::uint64_t> me_;
        bool warned_ = false;
    };

} // namespace

std::unique_ptr<Game> make_dice(Chat& chat, Terminal& terminal, Games& games) {
    return std::make_unique<Dice>(chat, terminal, games);
}

} // namespace zchat::game
