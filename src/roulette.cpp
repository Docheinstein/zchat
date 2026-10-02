// Roulette at the casino (/casino roulette): one European wheel (0 to 36) for the whole chat. /casino roulette bet N
// WHAT puts N coins on a number, red or black, odd or even, low (1 to 18) or high (19 to 36), a dozen or a column (see
// casino::roulette::parse()), as many bets as you like; whoever bets first when the wheel is free spins it, once the
// time to bet is up, or sooner when everybody who bet asks for it (/casino roulette spin: alone, right away).
// Everybody sees everybody's bets on the table. A number pays 35 to 1, a dozen or a column 2 to 1, the rest 1 to 1.
//
// The one who spins is the referee, and sends, as Game packets:
//   roulette open <round> <seconds>        the wheel takes bets, for so many more seconds (every couple of seconds)
//   roulette spin <round> <number>         where the ball stops (a few times)
// and everybody who bets sends:
//   roulette bet <round> <coins> <bet>     a bet (see casino::roulette::parse())
//   roulette ready <round>                 spin now (again until it spins)
// where <round> is a random hex number naming the round. Each zchat takes its user's bets, and pays their winnings
// itself when the ball stops; a wheel that never spins (its referee left) gives them back.

#include "casino_table.hpp"

#include <algorithm>
#include <deque>
#include <set>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    namespace rl = casino::roulette;

    constexpr std::string_view game_name = "roulette";
    constexpr auto bet_time = 25s;
    // How long the wheel turns, before everybody is told where the ball stopped.
    constexpr auto spin_time = 6s;
    constexpr auto heartbeat = 2s;
    constexpr auto patience = 8s;
    constexpr int final_sends = 3;
    constexpr std::size_t history_size = 12;

    std::string number_text(int n) {
        return std::format("{} {}", n, n == 0 ? "green" : rl::red(n) ? "red" : "black");
    }

    class Roulette final : public CasinoTable {
    public:
        Roulette(Chat& chat, Screen& terminal, Games& games) :
            CasinoTable(chat, terminal, games) {
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "bet on where the ball stops: a number pays 35 to 1, red or black 1 to 1";
        }

        std::vector<Help> help() const override {
            return {
                {"", "show the wheel and the bets"},
                {"bet N WHAT", "bet N coins on a number (0 to 36), red, black, odd, even, low, high, 1st, 2nd, 3rd "
                               "(dozens) or col1, col2, col3"},
                {"spin", "spin the wheel now, without waiting for more bets"},
            };
        }

        void start() override {
            if (terminal_.show_game("casino", "{\"open\":\"roulette\"}")) {
                return;
            }
            if (!live()) {
                chat_.notice(std::format("🎡 The roulette is free: /casino roulette bet N WHAT spins it.{}",
                                         history_.empty() ? "" : " Last numbers: " + history_text()));
                return;
            }
            chat_.notice(std::format("🎡 Roulette, spun by {} in {} seconds{}", colored(dealer_),
                                     seconds_left(deadline_), bets_.empty() ? "." : ":"));
            for (const Bet& b : bets_) {
                chat_.notice(std::format("   {}: {} on {}", colored(b.id), plural(b.amount, "coin"),
                                         rl::describe(b.bet)));
            }
        }

        bool command(std::string_view args) override {
            const std::string_view verb = next_field(args);
            if (verb == "bet") {
                const std::string_view coins = next_field(args);
                const auto bet = rl::parse(args);
                if (!bet) {
                    chat_.notice("🎡 Bet on a number (0 to 36), red, black, odd, even, low, high, 1st, 2nd, 3rd "
                                 "(dozens) or col1, col2, col3: like /casino roulette bet 10 red.");
                    return true;
                }
                if (const auto n = amount(coins)) {
                    place(*n, *bet);
                }
                return true;
            }
            if (verb == "spin" && args.empty()) {
                ready();
                return true;
            }
            return false;
        }

        void receive(std::uint64_t sender, std::string_view name, std::string_view text) override {
            heard(sender, name);
            const std::string_view event = next_field(text);
            const auto round = parse_number(next_field(text), 16);
            if (!round || *round == 0) {
                return;
            }
            if (event == "open") {
                if (const auto left = parse_number(next_field(text)); left && *left <= 600) {
                    opened(sender, *round, std::chrono::seconds(*left));
                }
                return;
            }
            if (*round != round_) {
                return;
            }
            if (event == "bet" && phase_ == 'b') {
                const auto coins = parse_number(next_field(text));
                const auto bet = rl::parse(text);
                const auto count = std::ranges::count(bets_, sender, &Bet::id);
                if (coins && *coins >= casino::min_bet && *coins <= casino::max_bet && bet &&
                    static_cast<std::size_t>(count) < rl::most_bets) {
                    bets_.push_back({sender, *bet, static_cast<long long>(*coins)});
                    chat_.notice(std::format("🎡 {} bets {} on {}.", colored(sender), plural(*coins, "coin"),
                                             rl::describe(*bet)));
                    publish();
                }
            } else if (event == "ready" && phase_ == 'b') {
                if (ready_.insert(sender).second) {
                    publish();
                }
                if (dealing_) {
                    spin_if_ready();
                }
            } else if (event == "spin" && sender == dealer_ && phase_ == 'b') {
                const auto number = parse_number(next_field(text));
                if (number && *number < rl::pockets) {
                    stopped(static_cast<int>(*number));
                }
            }
        }

        void tick() override {
            const auto now = clock::now();
            if (dealing_ && phase_ == 'b') {
                if (now >= deadline_) {
                    spin();
                } else if (now >= next_send_) {
                    tell();
                }
            } else if (dealing_ && finals_ > 0 && now >= next_send_) {
                --finals_;
                next_send_ = now + 1s;
                send(std::format("spin {:x} {}", round_, number_));
            }
            if (live() && !dealing_ && now >= heard_at_ + patience) {
                chat_.notice(std::format("🎡 The roulette's croupier, {}, is gone: no spin.", colored(dealer_)));
                give_back();
                round_ = 0;
                bets_.clear();
                publish();
            }
            if (live() && ready_.contains(chat_.id()) && !dealing_ && now >= next_ready_) {
                next_ready_ = now + heartbeat;
                send(std::format("ready {:x}", round_));
            }
            if (phase_ == 's' && now >= spun_at_ + spin_time) {
                reveal();
            }
            if (live() && seconds_left(deadline_) != shown_left_) {
                publish();
            }
        }

    private:
        struct Bet {
            std::uint64_t id = 0;
            std::string bet;
            long long amount = 0;
        };

        // Taking bets.
        bool live() const {
            return round_ != 0 && phase_ == 'b';
        }

        void place(long long coins, const std::string& bet) {
            if (round_ != 0 && phase_ == 's') {
                chat_.notice("🎡 The wheel is turning: wait for the next spin.");
                return;
            }
            if (live() && static_cast<std::size_t>(std::ranges::count(bets_, chat_.id(), &Bet::id)) >= rl::most_bets) {
                chat_.notice(std::format("🎡 That is {} bets already: enough for one spin.", rl::most_bets));
                return;
            }
            if (!games_.stake(coins, game_name)) {
                return;
            }
            if (!live()) {
                // A free wheel: we spin it.
                round_ = new_round();
                dealer_ = chat_.id();
                dealing_ = true;
                finals_ = 0;
                phase_ = 'b';
                bets_.clear();
                ready_.clear();
                staked_ = 0;
                deadline_ = clock::now() + bet_time;
                tell();
                chat_.notice(std::format("🎡 You open the roulette: the others have {} seconds to bet too (/casino "
                                         "roulette bet N WHAT), or /casino roulette spin spins it now.",
                                         bet_time.count()));
            }
            staked_ += coins;
            bets_.push_back({chat_.id(), bet, coins});
            send(std::format("bet {:x} {} {}", round_, coins, bet));
            chat_.notice(std::format("🎡 You bet {} on {}.", plural(coins, "coin"), rl::describe(bet)));
            publish();
        }

        void ready() {
            if (!live() || std::ranges::find(bets_, chat_.id(), &Bet::id) == bets_.end()) {
                chat_.notice("🎡 You have no bets on the wheel: /casino roulette bet N WHAT first.");
                return;
            }
            ready_.insert(chat_.id());
            if (dealing_) {
                spin_if_ready();
            } else {
                next_ready_ = clock::now() + heartbeat;
                send(std::format("ready {:x}", round_));
            }
            publish();
        }

        // The referee: spins once everybody who bet wants it.
        void spin_if_ready() {
            const bool all = std::ranges::all_of(bets_, [&](const Bet& b) {
                return ready_.contains(b.id);
            });
            if (all) {
                spin();
            }
        }

        void spin() {
            const int number = std::uniform_int_distribution<int>(0, rl::pockets - 1)(rng_);
            send(std::format("spin {:x} {}", round_, number));
            finals_ = final_sends - 1;
            next_send_ = clock::now() + 1s;
            stopped(number);
        }

        void tell() {
            send(std::format("open {:x} {}", round_, seconds_left(deadline_)));
            next_send_ = clock::now() + heartbeat;
        }

        // A wheel takes bets: ours, or another, which becomes ours when we have none (or when two opened at once,
        // the lowest numbered one, while they still take bets).
        void opened(std::uint64_t sender, std::uint64_t round, std::chrono::seconds left) {
            if (round != round_) {
                if (live() && round > round_) {
                    return;
                }
                if (live() && staked_ > 0) {
                    chat_.notice(std::format("🎡 Two roulettes opened at once: {}'s is the one. Bet again there.",
                                             colored(sender)));
                }
                if (live()) {
                    give_back();
                } else if (phase_ == 's') {
                    // Ours is still turning: settled first.
                    reveal();
                }
                round_ = round;
                dealer_ = sender;
                dealing_ = false;
                finals_ = 0;
                phase_ = 'b';
                bets_.clear();
                ready_.clear();
                staked_ = 0;
                chat_.notice(std::format("🎡 {} opens the roulette: /casino roulette bet N WHAT to bet too, /casino "
                                         "to see it.",
                                         colored(sender)));
            } else if (sender != dealer_ || dealing_ || phase_ != 'b') {
                return;
            }
            heard_at_ = clock::now();
            deadline_ = heard_at_ + left;
            publish();
        }

        // Where the ball stops: shown once the wheel has turned.
        void stopped(int number) {
            phase_ = 's';
            number_ = number;
            spun_at_ = clock::now();
            publish();
        }

        void reveal() {
            phase_ = 'o';
            history_.push_front(number_);
            if (history_.size() > history_size) {
                history_.pop_back();
            }
            // Each player's bets together.
            std::vector<std::uint64_t> players;
            for (const Bet& b : bets_) {
                if (std::ranges::find(players, b.id) == players.end()) {
                    players.push_back(b.id);
                }
            }
            std::string results;
            long long back = 0;
            for (const std::uint64_t id : players) {
                long long theirs = 0;
                long long staked = 0;
                for (const Bet& b : bets_) {
                    if (b.id == id) {
                        theirs += rl::payout(b.bet, b.amount, number_);
                        staked += b.amount;
                    }
                }
                if (id == chat_.id()) {
                    back = theirs;
                }
                results += std::format("{}{} {}", results.empty() ? "" : " · ", who(id, results.empty()),
                                       net_text(theirs - staked, staked, id == chat_.id()));
            }
            chat_.notice(std::format("🎡 {}! {}", number_text(number_), results));
            if (staked_ > 0) {
                games_.cash(back);
                chat_.notice(std::format("💰 You {} at roulette.", net_text(back - staked_, staked_)));
            }
            staked_ = 0;
            publish();
        }

        void give_back() {
            if (staked_ > 0) {
                games_.cash(staked_);
                chat_.notice(std::format("🎡 Your {} at roulette are back.", plural(staked_, "coin")));
            }
            staked_ = 0;
        }

        std::string history_text() const {
            std::string out;
            for (const int n : history_) {
                out += std::format("{}{}", out.empty() ? "" : ", ", n);
            }
            return out;
        }

        void publish() {
            shown_left_ = live() ? seconds_left(deadline_) : 0;
            const std::string_view phase = round_ == 0 ? "idle" : phase_ == 'b' ? "betting" : phase_ == 's' ? "spinning"
                                                                                                             : "over";
            std::string bets = "[";
            for (const Bet& b : bets_) {
                bets += std::format("{}{{{},\"bet\":{},\"label\":{},\"amount\":{},\"back\":{},\"ready\":{}}}",
                                    bets.size() > 1 ? "," : "", player_json(b.id), json(b.bet),
                                    json(rl::describe(b.bet)), b.amount,
                                    phase_ == 'o' ? std::to_string(rl::payout(b.bet, b.amount, number_)) : "null",
                                    ready_.contains(b.id));
            }
            bets += "]";
            std::string history = "[";
            for (const int n : history_) {
                history += std::format("{}{}", history.size() > 1 ? "," : "", n);
            }
            history += "]";
            const bool have_bets = std::ranges::find(bets_, chat_.id(), &Bet::id) != bets_.end();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - spun_at_);
            const std::string state = std::format(
                "{{\"round\":\"{:x}\",\"phase\":\"{}\",\"left\":{},\"dealer\":{},\"youDeal\":{},\"number\":{},"
                "\"spinMs\":{},\"spinElapsed\":{},\"history\":{},\"bets\":{},\"ready\":{},\"canBet\":{},"
                "\"canSpin\":{},\"staked\":{},\"min\":{},\"max\":{},\"betTime\":{}}}",
                round_, phase, shown_left_, json(round_ ? name_of(dealer_) : ""), dealing_,
                round_ != 0 && phase_ != 'b' ? std::to_string(number_) : "null",
                std::chrono::duration_cast<std::chrono::milliseconds>(spin_time).count(),
                phase_ == 's' ? elapsed.count() : 0, history, bets, ready_.contains(chat_.id()), phase_ != 's',
                live() && have_bets && !ready_.contains(chat_.id()), staked_, casino::min_bet, casino::max_bet,
                bet_time.count());
            terminal_.show_game(game_name, state);
        }

        std::uint64_t round_ = 0;
        std::uint64_t dealer_ = 0;
        bool dealing_ = false;
        // Taking bets ('b'), turning ('s'), or stopped ('o').
        char phase_ = 'o';
        clock::time_point deadline_;
        clock::time_point heard_at_;
        clock::time_point next_send_;
        clock::time_point next_ready_;
        int finals_ = 0;
        std::vector<Bet> bets_;
        // Who asked to spin.
        std::set<std::uint64_t> ready_;
        int number_ = 0;
        clock::time_point spun_at_;
        std::deque<int> history_;
        long long staked_ = 0;
        long long shown_left_ = -1;
    };

} // namespace

std::unique_ptr<Game> make_roulette(Chat& chat, Screen& terminal, Games& games) {
    return std::make_unique<Roulette>(chat, terminal, games);
}

} // namespace zchat::game
