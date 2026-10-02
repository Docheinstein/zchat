// The horse race at the casino (/casino horses): six horses, each with its odds, and the whole chat betting on them.
// /casino horses bet N HORSE puts N coins on a horse to win (by its number, or its name); whoever bets first when the
// track is free starts that race, once the time to bet is up, or sooner when everybody who bet asks for it (/casino
// horses go: alone, right away). Everybody sees everybody's bets, and the race, in the casino's window. A winning bet
// gives back its odds times the bet: the favourites pay little, the outsiders a lot, and on average the house keeps a
// tenth.
//
// Who starts the race is its referee: it picks the field, and how the race goes, and sends, as Game packets:
//   horses open <round> <seconds> <field>   the field (see casino::horses::encode_field()) takes bets, for so many
//                                            more seconds (every couple of seconds)
//   horses race <round> <order> <track>     how the race goes (see casino::horses::encode_race()): the order of
//                                            finish, and where each horse is at each checkpoint (a few times)
// and everybody who bets sends:
//   horses bet <round> <horse> <coins>      a bet on horse number <horse> (from 0)
//   horses ready <round>                    start now (again until it starts)
// where <round> is a random hex number naming the round. Every zchat shows the race as it goes, and once it is over
// pays its user's winnings; a race that never starts (its referee left) gives the bets back.

#include "casino_table.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    namespace hs = casino::horses;

    constexpr std::string_view game_name = "horses";
    constexpr auto bet_time = 25s;
    // How long a race takes: the checkpoints are evenly spread in it.
    constexpr auto race_time = 15s;
    constexpr auto heartbeat = 2s;
    constexpr auto patience = 8s;
    constexpr int final_sends = 3;

    std::string lowercase(std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        return out;
    }

    class Horses final : public CasinoTable {
    public:
        Horses(Chat& chat, Screen& terminal, Games& games) :
            CasinoTable(chat, terminal, games) {
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "bet on the horse that wins the race: the outsiders pay the most";
        }

        std::vector<Help> help() const override {
            return {
                {"", "show the horses, their odds and the bets"},
                {"open", "open a race: the horses and their odds, to bet on (betting opens one too)"},
                {"bet N HORSE", "bet N coins on a horse to win, by its number (1 to 6) or its name"},
                {"go", "start the race now, without waiting for more bets"},
            };
        }

        void start() override {
            if (terminal_.show_game("casino", "{\"open\":\"horses\"}")) {
                return;
            }
            if (!live()) {
                chat_.game_notice(phase_ == 'r' ? "🏇 A race is running: wait for the next one."
                                                : "🏇 The track is free: /casino horses open starts a race, with its "
                                                  "horses and their odds.");
                return;
            }
            chat_.game_notice(std::format("🏇 The horses, off in {} seconds (started by {}):", seconds_left(deadline_),
                                          colored(dealer_)));
            for (std::size_t i = 0; i < field_.size(); ++i) {
                std::string bets;
                for (const Bet& b : bets_) {
                    if (b.horse == i) {
                        bets += std::format("{}{} {}", bets.empty() ? " · " : ", ", colored(b.id), b.amount);
                    }
                }
                chat_.game_notice(
                    std::format("   {}. {} x{}{}", i + 1, horse_name(i), hs::odds_text(field_[i].odds), bets));
            }
        }

        bool command(std::string_view args) override {
            const std::string_view verb = next_field(args);
            if (verb == "bet") {
                const std::string_view coins = next_field(args);
                if (!live() && phase_ != 'r') {
                    // A free track: the horses come with the race, so the bet is on a number.
                    const auto n = parse_number(args);
                    if (!n || *n < 1 || *n > static_cast<std::uint64_t>(hs::runners)) {
                        chat_.game_notice(
                            std::format("🏇 The track is free: /casino horses bet N HORSE starts a race, "
                                        "with HORSE a number from 1 to {}; the horses and their odds come "
                                        "with it.",
                                        hs::runners));
                        return true;
                    }
                    if (const auto c = amount(coins); c && games_.stake(*c, "the horse race")) {
                        open_race();
                        add_own(*c, static_cast<std::size_t>(*n - 1));
                    }
                    return true;
                }
                const auto horse = find_horse(args);
                if (!horse) {
                    chat_.game_notice(std::format(
                        "🏇 No horse {} in this race: bet on one by number (1 to {}) or name.", args, field_.size()));
                    return true;
                }
                if (const auto c = amount(coins)) {
                    place(*c, *horse);
                }
                return true;
            }
            if (verb == "open" && args.empty()) {
                if (live() || phase_ == 'r') {
                    chat_.game_notice(live() ? "🏇 A race is open: /casino horses bet N HORSE to bet on it."
                                             : "🏇 A race is running: wait for the next one.");
                } else {
                    open_race();
                    start();
                }
                return true;
            }
            if (verb == "go" && args.empty()) {
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
                const auto left = parse_number(next_field(text));
                auto field = hs::decode_field(text);
                if (left && *left <= 600 && field) {
                    opened(sender, *round, std::chrono::seconds(*left), std::move(*field));
                }
                return;
            }
            if (*round != round_) {
                return;
            }
            if (event == "bet" && phase_ == 'b') {
                const auto horse = parse_number(next_field(text));
                const auto coins = parse_number(next_field(text));
                if (horse && *horse < field_.size() && coins && *coins >= casino::min_bet &&
                    *coins <= casino::max_bet) {
                    bets_.push_back({sender, static_cast<std::size_t>(*horse), static_cast<long long>(*coins)});
                    chat_.game_notice(std::format("🏇 {} bets {} on {} (x{}).", colored(sender), plural(*coins, "coin"),
                                                  horse_name(*horse), hs::odds_text(field_[*horse].odds)));
                    publish();
                }
            } else if (event == "ready" && phase_ == 'b') {
                if (ready_.insert(sender).second) {
                    publish();
                }
                if (dealing_) {
                    go_if_ready();
                }
            } else if (event == "race" && sender == dealer_ && phase_ == 'b') {
                if (auto race = hs::decode_race(text, field_.size())) {
                    off(std::move(*race));
                }
            }
        }

        void tick() override {
            const auto now = clock::now();
            if (dealing_ && phase_ == 'b') {
                if (now >= deadline_) {
                    run();
                } else if (now >= next_send_) {
                    tell();
                }
            } else if (dealing_ && finals_ > 0 && now >= next_send_) {
                --finals_;
                next_send_ = now + 1s;
                send(std::format("race {:x} {}", round_, hs::encode_race(race_->order, race_->track)));
            }
            if (live() && !dealing_ && now >= heard_at_ + patience) {
                chat_.game_notice(std::format("🏇 The race's starter, {}, is gone: no race.", colored(dealer_)));
                give_back();
                round_ = 0;
                phase_ = 'o';
                bets_.clear();
                publish();
            }
            if (live() && ready_.contains(chat_.id()) && !dealing_ && now >= next_ready_) {
                next_ready_ = now + heartbeat;
                send(std::format("ready {:x}", round_));
            }
            if (phase_ == 'r') {
                if (!halfway_ && now >= started_ + race_time / 2) {
                    halfway_ = true;
                    halfway();
                }
                if (now >= started_ + race_time) {
                    finish();
                }
            }
            if (live() && seconds_left(deadline_) != shown_left_) {
                publish();
            }
        }

    private:
        struct Bet {
            std::uint64_t id = 0;
            std::size_t horse = 0;
            long long amount = 0;
        };

        bool live() const {
            return round_ != 0 && phase_ == 'b';
        }

        std::string horse_name(std::size_t i) const {
            return i < field_.size() ? std::string(hs::names[static_cast<std::size_t>(field_[i].name)]) : "?";
        }

        // A horse as typed: its number, or (the start of) its name.
        std::optional<std::size_t> find_horse(std::string_view typed) const {
            if (const auto n = parse_number(typed); n && *n >= 1 && *n <= field_.size()) {
                return static_cast<std::size_t>(*n - 1);
            }
            const std::string wanted = lowercase(typed);
            if (wanted.empty()) {
                return std::nullopt;
            }
            for (std::size_t i = 0; i < field_.size(); ++i) {
                if (lowercase(horse_name(i)).starts_with(wanted)) {
                    return i;
                }
            }
            return std::nullopt;
        }

        // A free track: we start a race.
        void open_race() {
            auto f = hs::field(rng_);
            round_ = new_round();
            dealer_ = chat_.id();
            dealing_ = true;
            finals_ = 0;
            phase_ = 'b';
            field_ = std::move(f.horses);
            weights_ = std::move(f.weights);
            race_.reset();
            bets_.clear();
            ready_.clear();
            staked_ = 0;
            deadline_ = clock::now() + bet_time;
            tell();
            chat_.game_notice(std::format("🏇 You open a horse race: everybody has {} seconds to bet (/casino horses "
                                          "bet N HORSE), or /casino horses go starts it now.",
                                          bet_time.count()));
            publish();
        }

        void place(long long coins, std::size_t horse) {
            if (phase_ == 'r') {
                chat_.game_notice("🏇 The race is on: wait for the next one.");
                return;
            }
            if (!games_.stake(coins, "the horse race")) {
                return;
            }
            add_own(coins, horse);
        }

        void add_own(long long coins, std::size_t horse) {
            staked_ += coins;
            bets_.push_back({chat_.id(), horse, coins});
            send(std::format("bet {:x} {} {}", round_, horse, coins));
            chat_.game_notice(std::format("🏇 You bet {} on {} (x{}: it would give back {}).", plural(coins, "coin"),
                                          horse_name(horse), hs::odds_text(field_[horse].odds),
                                          hs::payout(coins, field_[horse].odds)));
            publish();
        }

        void ready() {
            if (!live() || std::ranges::find(bets_, chat_.id(), &Bet::id) == bets_.end()) {
                chat_.game_notice("🏇 You have no bets on this race: /casino horses bet N HORSE first.");
                return;
            }
            ready_.insert(chat_.id());
            if (dealing_) {
                go_if_ready();
            } else {
                next_ready_ = clock::now() + heartbeat;
                send(std::format("ready {:x}", round_));
            }
            publish();
        }

        void go_if_ready() {
            const bool all = std::ranges::all_of(bets_, [&](const Bet& b) {
                return ready_.contains(b.id);
            });
            if (all) {
                run();
            }
        }

        // The referee: how the race goes.
        void run() {
            hs::Race race;
            race.order = hs::finish_order(weights_, rng_);
            race.track = hs::track(race.order, rng_);
            send(std::format("race {:x} {}", round_, hs::encode_race(race.order, race.track)));
            finals_ = final_sends - 1;
            next_send_ = clock::now() + 1s;
            off(std::move(race));
        }

        void tell() {
            send(std::format("open {:x} {} {}", round_, seconds_left(deadline_), hs::encode_field(field_)));
            next_send_ = clock::now() + heartbeat;
        }

        void opened(std::uint64_t sender, std::uint64_t round, std::chrono::seconds left, std::vector<hs::Horse> field) {
            if (round != round_) {
                if ((live() && round > round_) || phase_ == 'r') {
                    return;
                }
                if (live() && staked_ > 0) {
                    chat_.game_notice(
                        std::format("🏇 Two races opened at once: {}'s is the one. Bet again there.", colored(sender)));
                }
                give_back();
                round_ = round;
                dealer_ = sender;
                dealing_ = false;
                finals_ = 0;
                phase_ = 'b';
                field_ = std::move(field);
                race_.reset();
                bets_.clear();
                ready_.clear();
                chat_.game_notice(std::format("🏇 {} opens a horse race: /casino horses bet N HORSE to bet, /casino to "
                                              "see it. Favourite: {} (x{}).",
                                              colored(sender), horse_name(favourite()),
                                              hs::odds_text(field_[favourite()].odds)));
            } else if (sender != dealer_ || dealing_ || phase_ != 'b') {
                return;
            }
            heard_at_ = clock::now();
            deadline_ = heard_at_ + left;
            publish();
        }

        std::size_t favourite() const {
            return static_cast<std::size_t>(std::ranges::min_element(field_, {}, &hs::Horse::odds) - field_.begin());
        }

        // They're off.
        void off(hs::Race race) {
            phase_ = 'r';
            race_ = std::move(race);
            started_ = clock::now();
            halfway_ = false;
            chat_.game_notice("🏇 And they're off!");
            terminal_.bell();
            publish();
        }

        // The horses by where they are at the checkpoint, the first first.
        std::vector<std::size_t> standings(std::size_t checkpoint) const {
            std::vector<std::size_t> out(field_.size());
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[i] = i;
            }
            std::ranges::stable_sort(out, std::ranges::greater {}, [&](std::size_t i) {
                return race_->track[i][checkpoint];
            });
            return out;
        }

        void halfway() {
            const auto at = standings(hs::checkpoints / 2 - 1);
            chat_.game_notice(std::format("🏇 Halfway: {} leads, then {} and {}.", horse_name(at[0]), horse_name(at[1]),
                                          horse_name(at[2])));
        }

        void finish() {
            phase_ = 'o';
            const auto& order = race_->order;
            const std::size_t winner = static_cast<std::size_t>(order.front());
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
                        theirs += b.horse == winner ? hs::payout(b.amount, field_[winner].odds) : 0;
                        staked += b.amount;
                    }
                }
                if (id == chat_.id()) {
                    back = theirs;
                }
                results += std::format("{}{} {}", results.empty() ? "" : " · ", who(id, results.empty()),
                                       net_text(theirs - staked, staked, id == chat_.id()));
            }
            chat_.game_notice(
                std::format("🏁 {} wins (x{})! Then {}, {}.{}", horse_name(winner), hs::odds_text(field_[winner].odds),
                            horse_name(static_cast<std::size_t>(order[1])),
                            horse_name(static_cast<std::size_t>(order[2])), results.empty() ? "" : " " + results));
            if (staked_ > 0) {
                games_.cash(back);
                chat_.game_notice(std::format("💰 You {} at the races.", net_text(back - staked_, staked_)));
            }
            staked_ = 0;
            publish();
        }

        void give_back() {
            if (staked_ > 0) {
                games_.cash(staked_);
                chat_.game_notice(std::format("🏇 Your {} on the race are back.", plural(staked_, "coin")));
            }
            staked_ = 0;
        }

        void publish() {
            shown_left_ = live() ? seconds_left(deadline_) : 0;
            const std::string_view phase = round_ == 0 ? "idle" : phase_ == 'b' ? "betting" : phase_ == 'r' ? "running"
                                                                                                             : "over";
            static constexpr std::array colors {"#e5534b", "#6ea8ff", "#3fb950", "#f2c94c", "#c678dd", "#ff9f43"};
            std::string horses = "[";
            for (std::size_t i = 0; i < field_.size(); ++i) {
                horses += std::format("{}{{\"n\":{},\"name\":{},\"odds\":\"{}\",\"color\":\"{}\"}}",
                                      horses.size() > 1 ? "," : "", i + 1, json(horse_name(i)),
                                      hs::odds_text(field_[i].odds), colors[i % colors.size()]);
            }
            horses += "]";
            std::string track = "null";
            std::string order = "null";
            if (race_) {
                track = "[";
                for (const auto& positions : race_->track) {
                    std::string row = "[";
                    for (const int at : positions) {
                        row += std::format("{}{}", row.size() > 1 ? "," : "", at);
                    }
                    track += std::format("{}{}]", track.size() > 1 ? "," : "", row);
                }
                track += "]";
                order = "[";
                for (const int i : race_->order) {
                    order += std::format("{}{}", order.size() > 1 ? "," : "", i);
                }
                order += "]";
            }
            const std::size_t winner = race_ ? static_cast<std::size_t>(race_->order.front()) : field_.size();
            std::string bets = "[";
            for (const Bet& b : bets_) {
                bets += std::format("{}{{{},\"horse\":{},\"amount\":{},\"back\":{},\"ready\":{}}}",
                                    bets.size() > 1 ? "," : "", player_json(b.id), b.horse, b.amount,
                                    phase_ == 'o' && race_
                                        ? std::to_string(b.horse == winner ? hs::payout(b.amount, field_[winner].odds)
                                                                           : 0)
                                        : "null",
                                    ready_.contains(b.id));
            }
            bets += "]";
            const bool have_bets = std::ranges::find(bets_, chat_.id(), &Bet::id) != bets_.end();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - started_);
            const std::string state = std::format(
                "{{\"round\":\"{:x}\",\"phase\":\"{}\",\"left\":{},\"dealer\":{},\"youDeal\":{},\"horses\":{},"
                "\"track\":{},\"order\":{},\"raceMs\":{},\"elapsed\":{},\"checkpoints\":{},\"finish\":{},"
                "\"bets\":{},\"ready\":{},\"canBet\":{},\"canGo\":{},\"staked\":{},\"min\":{},\"max\":{},"
                "\"runners\":{},\"betTime\":{}}}",
                round_, phase, shown_left_, json(round_ ? name_of(dealer_) : ""), dealing_, horses, track, order,
                std::chrono::duration_cast<std::chrono::milliseconds>(race_time).count(),
                phase_ == 'r' ? elapsed.count() : 0, hs::checkpoints, hs::finish, bets, ready_.contains(chat_.id()),
                phase_ != 'r', live() && have_bets && !ready_.contains(chat_.id()), staked_, casino::min_bet,
                casino::max_bet, hs::runners, bet_time.count());
            terminal_.show_game(game_name, state);
        }

        std::uint64_t round_ = 0;
        std::uint64_t dealer_ = 0;
        bool dealing_ = false;
        // Taking bets ('b'), running ('r'), or over ('o').
        char phase_ = 'o';
        std::vector<hs::Horse> field_;
        // The referee's: how likely each horse is to win.
        std::vector<double> weights_;
        std::optional<hs::Race> race_;
        clock::time_point started_;
        bool halfway_ = false;
        clock::time_point deadline_;
        clock::time_point heard_at_;
        clock::time_point next_send_;
        clock::time_point next_ready_;
        int finals_ = 0;
        std::vector<Bet> bets_;
        std::set<std::uint64_t> ready_;
        long long staked_ = 0;
        long long shown_left_ = -1;
    };

} // namespace

std::unique_ptr<Game> make_horses(Chat& chat, Screen& terminal, Games& games) {
    return std::make_unique<Horses>(chat, terminal, games);
}

} // namespace zchat::game
