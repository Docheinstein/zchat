// Blackjack at the casino (/casino blackjack): one table for the whole chat, of up to seven players against the dealer.
// /casino blackjack bet N sits down with a bet of N coins; whoever bets first at a free table deals that round. The
// others have some time to bet too, then the cards are dealt (sooner when everybody seated asks for them, with /casino
// blackjack deal: alone, right away). Everybody plays their hand at the same time, seeing all the others':
// /casino blackjack hit, stand, double (twice the bet, one card more) or split (two cards of the same value, once).
// The dealer draws to 16 and stands on all 17s; a blackjack pays 3 to 2, a win 1 to 1, a push gives the bet back.
// If the dealer's first card is an ace or worth ten, the dealer looks at the second one: with a blackjack, the round
// ends right away.
//
// The dealer's zchat is the referee: it shuffles six decks, deals, and tells everybody the table, as a Game packet:
//   blackjack state <round> <table>          the table (see casino::blackjack::encode()), on every change and every
//                                            couple of seconds; the end a few times
// and the players send it:
//   blackjack bet <round> <coins>            sits down with a bet
//   blackjack ready <round>                  deal now
//   blackjack act <round> <hand> <cards> hit|stand|double|split   a move on their hand number <hand> (0, or 1 after a
//                                            split), which had <cards> cards: a move sent again is not made twice
// each sent again until the table shows it. <round> is a random hex number naming the round. Each player's zchat
// takes their bets, and pays their winnings from the table as it ends; whatever the dealer did not take (a bet heard
// too late, a double not made) is given back. Two tables opened at the same time become the one with the lowest
// number, before the deal; a dealer who goes quiet gives everybody their bets back.

#include "casino_table.hpp"

#include <algorithm>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    namespace bj = casino::blackjack;
    using casino::Card;

    constexpr std::string_view game_name = "blackjack";
    constexpr auto bet_time = 20s;
    constexpr auto play_time = 30s;
    // How often the dealer tells the table, a packet not seen yet is sent again, and when the dealer is given up.
    constexpr auto heartbeat = 2s;
    constexpr auto resend_every = 1500ms;
    constexpr auto patience = 8s;
    // The end of a round is sent this many times.
    constexpr int final_sends = 3;

    std::string hand_text(const std::vector<Card>& cards) {
        std::string out;
        for (const Card c : cards) {
            out += (out.empty() ? "" : " ") + casino::card_label(c);
        }
        return out;
    }

    class Blackjack final : public CasinoTable {
    public:
        Blackjack(Chat& chat, Screen& terminal, Games& games) :
            CasinoTable(chat, terminal, games) {
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "beat the dealer to 21 without going over: a blackjack pays 3 to 2";
        }

        std::vector<Help> help() const override {
            return {
                {"", "show the table"},
                {"bet N", "sit down with a bet of N coins (the first to bet deals, and the others can join)"},
                {"deal", "deal the cards now, without waiting for more players"},
                {"hit", "take a card"},
                {"stand", "keep your hand"},
                {"double", "double your bet, for one more card"},
                {"split", "two cards of the same value: play them as two hands, with a bet each"},
            };
        }

        // /casino blackjack: the table, in the casino's window or in the chat.
        void start() override {
            if (terminal_.show_game("casino", "{\"open\":\"blackjack\"}")) {
                return;
            }
            if (!live()) {
                chat_.notice("🃏 The blackjack table is free: /casino blackjack bet N sits down with N coins, and "
                             "deals.");
                return;
            }
            chat_.notice(std::format("🃏 Blackjack, dealt by {}: {}", colored(dealer_),
                                     table_.phase == 'b' ? std::format("taking bets for {} more seconds", left())
                                                         : std::format("being played, {} seconds left", left())));
            if (table_.phase == 'p') {
                chat_.notice(std::format("   The dealer shows {}.", hand_text(table_.dealer)));
            }
            for (const bj::Seat& s : table_.seats) {
                chat_.notice(std::format("   {}: {}{}", colored(s.id), plural(s.bet, "coin"),
                                         s.hands.empty() ? "" : " · " + hands_text(s)));
            }
        }

        bool command(std::string_view args) override {
            const std::string_view verb = next_field(args);
            if (verb == "bet") {
                if (const auto coins = amount(args)) {
                    bet(*coins);
                }
                return true;
            }
            if (verb == "deal" && args.empty()) {
                ready();
                return true;
            }
            if ((verb == "hit" || verb == "stand" || verb == "double" || verb == "split") && args.empty()) {
                move(verb);
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
            if (event == "state") {
                if (auto table = bj::decode(text)) {
                    told(sender, *round, std::move(*table));
                }
                return;
            }
            // The rest is for the dealer.
            if (!dealing_ || *round != round_) {
                return;
            }
            if (event == "bet") {
                const auto coins = parse_number(next_field(text));
                if (coins && *coins >= casino::min_bet && *coins <= casino::max_bet) {
                    seat(sender, static_cast<long long>(*coins));
                }
            } else if (event == "ready") {
                ready_from(sender);
            } else if (event == "act") {
                const auto hand = parse_number(next_field(text));
                const auto cards = parse_number(next_field(text));
                if (hand && cards) {
                    act(sender, *hand, *cards, text);
                }
            }
        }

        void tick() override {
            const auto now = clock::now();
            if (dealing_) {
                if (live() && now >= deadline_) {
                    table_.phase == 'b' ? deal() : dealer_plays();
                } else if (live() && now >= next_send_) {
                    tell();
                } else if (!live() && finals_ > 0 && now >= next_send_) {
                    --finals_;
                    tell();
                }
            } else if (live() && now >= heard_at_ + patience) {
                chat_.notice(std::format("🃏 The blackjack dealer, {}, is gone: the round is off.", colored(dealer_)));
                give_back();
                table_ = {};
                round_ = 0;
                publish();
            }
            if (pending_ && now >= pending_->sent + resend_every) {
                pending_->sent = now;
                send(pending_->text);
            }
            if (live() && left() != shown_left_) {
                publish();
            }
        }

    private:
        // A packet of ours sent again until the table shows it.
        struct Pending {
            std::string text;
            clock::time_point sent;
        };

        // A round is on, and not over.
        bool live() const {
            return round_ != 0 && table_.phase != 'o';
        }

        long long left() const {
            return seconds_left(deadline_);
        }

        const bj::Seat* own_seat() const {
            const auto it = std::ranges::find(table_.seats, chat_.id(), &bj::Seat::id);
            return it == table_.seats.end() ? nullptr : &*it;
        }

        // Our first hand still to play, or nullopt.
        std::optional<std::size_t> own_hand() const {
            const bj::Seat* s = own_seat();
            if (!s || table_.phase != 'p') {
                return std::nullopt;
            }
            for (std::size_t i = 0; i < s->hands.size(); ++i) {
                if (!s->hands[i].done) {
                    return i;
                }
            }
            return std::nullopt;
        }

        void bet(long long coins) {
            if (live() && table_.phase == 'p') {
                chat_.notice("🃏 The cards are being played: wait for the next round to bet.");
                return;
            }
            if (live() && own_seat()) {
                chat_.notice(std::format("🃏 You are seated with {}: /casino blackjack deal deals the cards.",
                                         plural(own_seat()->bet, "coin")));
                return;
            }
            if (live() && table_.seats.size() >= bj::seats) {
                chat_.notice("🃏 The blackjack table is full: wait for the next round.");
                return;
            }
            if (live() && pending_) {
                chat_.notice("🃏 Your bet is on its way to the dealer.");
                return;
            }
            if (!games_.stake(coins, game_name)) {
                return;
            }
            staked_ = coins;
            if (!live()) {
                // A free table: we deal.
                round_ = new_round();
                dealer_ = chat_.id();
                dealing_ = true;
                finals_ = 0;
                pending_.reset();
                table_ = {};
                deadline_ = clock::now() + bet_time;
                chat_.notice(std::format("🃏 You open the blackjack table with {}: the others have {} seconds to bet "
                                         "(/casino blackjack bet N), or /casino blackjack deal deals now.",
                                         plural(coins, "coin"), bet_time.count()));
                bj::Table next = table_;
                next.seats.push_back({chat_.id(), coins, false, {}});
                commit(std::move(next));
                return;
            }
            chat_.notice(std::format("🃏 You bet {} at blackjack.", plural(coins, "coin")));
            if (dealing_) {
                seat(chat_.id(), coins);
            } else {
                post(std::format("bet {:x} {}", round_, coins));
            }
        }

        void ready() {
            const bj::Seat* s = own_seat();
            if (!live() || table_.phase != 'b' || !s) {
                chat_.notice(live() && table_.phase == 'p' ? "🃏 The cards are dealt already."
                                                           : "🃏 You are not seated: /casino blackjack bet N first.");
                return;
            }
            if (dealing_) {
                ready_from(chat_.id());
            } else if (!s->ready) {
                post(std::format("ready {:x}", round_));
            }
        }

        void move(std::string_view verb) {
            const auto hand = own_hand();
            if (!hand) {
                chat_.notice(live() && table_.phase == 'p' && own_seat()
                                 ? "🃏 Your hand is played: wait for the dealer."
                                 : "🃏 You have no hand to play: /casino blackjack bet N sits you down for the next "
                                   "round.");
                return;
            }
            const bj::Seat& s = *own_seat();
            const bj::Hand& h = s.hands[*hand];
            if (verb == "double" && (h.cards.size() != 2 || h.doubled)) {
                chat_.notice("🃏 Only a hand of two cards can be doubled.");
                return;
            }
            if (verb == "split" && (s.hands.size() != 1 || !bj::can_split(h.cards))) {
                chat_.notice("🃏 Only two cards of the same value can be split, once.");
                return;
            }
            if (pending_) {
                chat_.notice("🃏 Your last move is on its way to the dealer.");
                return;
            }
            if (verb == "double" || verb == "split") {
                if (!games_.stake(s.bet, game_name)) {
                    return;
                }
                staked_ += s.bet;
            }
            if (dealing_) {
                act(chat_.id(), *hand, h.cards.size(), verb);
            } else {
                post(std::format("act {:x} {} {} {}", round_, *hand, h.cards.size(), verb));
            }
        }

        void post(std::string text) {
            send(text);
            pending_ = Pending {std::move(text), clock::now()};
        }

        // Dealer: somebody sits down.
        void seat(std::uint64_t id, long long coins) {
            if (table_.phase != 'b') {
                return;
            }
            bj::Table next = table_;
            if (std::ranges::find(next.seats, id, &bj::Seat::id) == next.seats.end()) {
                if (next.seats.size() >= bj::seats) {
                    return;
                }
                next.seats.push_back({id, coins, false, {}});
            }
            commit(std::move(next));
        }

        // Dealer: somebody wants the cards dealt; once all do, they are.
        void ready_from(std::uint64_t id) {
            if (table_.phase != 'b') {
                return;
            }
            bj::Table next = table_;
            const auto it = std::ranges::find(next.seats, id, &bj::Seat::id);
            if (it == next.seats.end()) {
                return;
            }
            it->ready = true;
            if (std::ranges::all_of(next.seats, &bj::Seat::ready)) {
                table_ = std::move(next);
                deal();
                return;
            }
            commit(std::move(next));
        }

        Card draw() {
            if (shoe_.empty()) {
                shoe_ = casino::shoe(bj::decks, rng_);
            }
            const Card c = shoe_.back();
            shoe_.pop_back();
            return c;
        }

        // Dealer: the cards.
        void deal() {
            if (table_.seats.empty()) {
                round_ = 0;
                table_ = {};
                publish();
                return;
            }
            shoe_ = casino::shoe(bj::decks, rng_);
            bj::Table next = table_;
            next.phase = 'p';
            for (bj::Seat& s : next.seats) {
                s.hands = {bj::Hand {{draw()}}};
            }
            const Card up = draw();
            for (bj::Seat& s : next.seats) {
                s.hands[0].cards.push_back(draw());
                s.hands[0].done = bj::natural(s.hands[0].cards);
            }
            hole_ = draw();
            next.dealer = {up, casino::hidden};
            deadline_ = clock::now() + play_time;
            // The dealer looks at the hole card when the first one could make a blackjack.
            if (bj::natural({up, hole_})) {
                table_ = std::move(next);
                dealer_plays();
                return;
            }
            if (all_done(next)) {
                table_ = std::move(next);
                dealer_plays();
                return;
            }
            commit(std::move(next));
        }

        static bool all_done(const bj::Table& t) {
            return std::ranges::all_of(t.seats, [](const bj::Seat& s) {
                return std::ranges::all_of(s.hands, &bj::Hand::done);
            });
        }

        // Dealer: a player's move, made once.
        void act(std::uint64_t id, std::uint64_t hand, std::uint64_t cards, std::string_view verb) {
            if (table_.phase != 'p') {
                return;
            }
            bj::Table next = table_;
            const auto seat = std::ranges::find(next.seats, id, &bj::Seat::id);
            if (seat == next.seats.end() || hand >= seat->hands.size() || seat->hands[hand].done ||
                seat->hands[hand].cards.size() != cards) {
                // Made already, or not to be made: they need to see the table.
                tell();
                return;
            }
            bj::Hand& h = seat->hands[hand];
            const auto finished = [](bj::Hand& x) {
                x.done = x.done || bj::total(x.cards).value >= 21;
            };
            if (verb == "hit") {
                h.cards.push_back(draw());
                finished(h);
            } else if (verb == "stand") {
                h.done = true;
            } else if (verb == "double" && h.cards.size() == 2 && !h.doubled) {
                h.doubled = true;
                h.cards.push_back(draw());
                h.done = true;
            } else if (verb == "split" && seat->hands.size() == 1 && bj::can_split(h.cards)) {
                const bool aces = h.cards[0] % 13 == 0;
                bj::Hand a {{h.cards[0], draw()}};
                bj::Hand b {{h.cards[1], draw()}};
                for (bj::Hand* x : {&a, &b}) {
                    // Split aces get one card each.
                    x->done = aces;
                    finished(*x);
                }
                seat->hands = {a, b};
            } else {
                tell();
                return;
            }
            if (all_done(next)) {
                table_ = std::move(next);
                dealer_plays();
                return;
            }
            commit(std::move(next));
        }

        // Dealer: everybody played (or time is up): the dealer's turn, and the end.
        void dealer_plays() {
            bj::Table next = table_;
            for (bj::Seat& s : next.seats) {
                for (bj::Hand& h : s.hands) {
                    h.done = true;
                }
            }
            if (next.dealer.size() == 2 && next.dealer[1] == casino::hidden) {
                next.dealer[1] = hole_;
            }
            // Against busted hands and blackjacks only, the dealer does not draw.
            const bool against = std::ranges::any_of(next.seats, [](const bj::Seat& s) {
                return std::ranges::any_of(s.hands, [&](const bj::Hand& h) {
                    return bj::total(h.cards).value <= 21 && !(s.hands.size() == 1 && bj::natural(h.cards));
                });
            });
            while (against && !bj::natural(next.dealer) && bj::dealer_draws(next.dealer)) {
                next.dealer.push_back(draw());
            }
            next.phase = 'o';
            finals_ = final_sends - 1;
            commit(std::move(next));
        }

        // Dealer: the table changes, and everybody is told.
        void commit(bj::Table next) {
            update(chat_.id(), std::move(next));
            tell();
        }

        void tell() {
            table_.left = static_cast<int>(left());
            send(std::format("state {:x} {}", round_, bj::encode(table_)));
            next_send_ = clock::now() + (table_.phase == 'o' ? 1s : heartbeat);
        }

        // The dealer told the table.
        void told(std::uint64_t sender, std::uint64_t round, bj::Table table) {
            if (round != round_) {
                // Two tables opened at once: the lowest is the one, before the cards are dealt. A table seen while
                // ours is over (or we have none) is the new one.
                const bool ours_open = live() && table_.phase == 'b';
                if (live() && !(ours_open && round < round_)) {
                    return;
                }
                if (live() && staked_ > 0) {
                    chat_.notice(std::format("🃏 Two blackjack tables opened at once: {}'s is the one. Bet again "
                                             "there.",
                                             colored(sender)));
                }
                give_back();
                round_ = round;
                dealer_ = sender;
                dealing_ = false;
                finals_ = 0;
                table_ = {};
                // The end of a round we did not see: only to show it.
                if (table.phase == 'o') {
                    table_ = std::move(table);
                    publish();
                    return;
                }
            } else if (dealing_ || sender != dealer_) {
                return;
            }
            heard_at_ = clock::now();
            deadline_ = heard_at_ + std::chrono::seconds(table.left);
            update(sender, std::move(table));
        }

        // The table as it is now: says what changed, and pays us at the end.
        void update(std::uint64_t dealer, bj::Table next) {
            const bj::Table before = std::move(table_);
            table_ = std::move(next);
            const bool fresh = before.seats.empty() && before.dealer.empty();
            if (fresh && table_.phase == 'b' && dealer != chat_.id()) {
                const auto theirs = std::ranges::find(table_.seats, dealer, &bj::Seat::id);
                chat_.notice(std::format("🃏 {} opens the blackjack table{}: /casino blackjack bet N to sit down, "
                                         "/casino to see it.",
                                         colored(dealer),
                                         theirs == table_.seats.end()
                                             ? ""
                                             : std::format(" with a bet of {}", plural(theirs->bet, "coin"))));
            }
            if (table_.phase == 'b') {
                for (const bj::Seat& s : table_.seats) {
                    if (s.id != chat_.id() && s.id != dealer &&
                        std::ranges::find(before.seats, s.id, &bj::Seat::id) == before.seats.end()) {
                        chat_.notice(std::format("🃏 {} bets {} at blackjack.", colored(s.id), plural(s.bet, "coin")));
                    }
                }
            }
            if (pending_ && seen(*pending_)) {
                pending_.reset();
            }
            const bj::Seat* mine = own_seat();
            const auto was = std::ranges::find(before.seats, chat_.id(), &bj::Seat::id);
            if (table_.phase == 'p' && mine) {
                const bool changed = was == before.seats.end() || before.phase != 'p' ||
                                     hands_text(*was) != hands_text(*mine);
                if (changed) {
                    const auto hand = own_hand();
                    chat_.notice(std::format("🃏 {} · the dealer shows {}{}", hands_text(*mine),
                                             casino::card_label(table_.dealer.front()),
                                             hand ? std::format(". /casino blackjack hit, stand{}{}",
                                                                mine->hands[*hand].cards.size() == 2 ? ", double" : "",
                                                                mine->hands.size() == 1 &&
                                                                        bj::can_split(mine->hands[0].cards)
                                                                    ? ", split"
                                                                    : "")
                                                  : ""));
                    if (hand) {
                        terminal_.bell();
                    }
                }
            }
            if (table_.phase == 'o' && before.phase != 'o') {
                finish();
            }
            publish();
        }

        // Whether the table shows a packet of ours.
        bool seen(const Pending& p) const {
            const bj::Seat* mine = own_seat();
            if (p.text.starts_with("bet ")) {
                return mine || table_.phase != 'b';
            }
            if (p.text.starts_with("ready ")) {
                return (mine && mine->ready) || table_.phase != 'b';
            }
            // A move: its hand has more cards, or is done, or the round is past it.
            std::string_view rest = p.text;
            next_field(rest);
            next_field(rest);
            const auto hand = parse_number(next_field(rest));
            const auto cards = parse_number(next_field(rest));
            return table_.phase != 'p' || !mine || !hand || !cards || *hand >= mine->hands.size() ||
                   mine->hands[*hand].done || mine->hands[*hand].cards.size() != *cards ||
                   (rest == "split" && mine->hands.size() == 2);
        }

        std::string hands_text(const bj::Seat& s) const {
            std::string out;
            for (const bj::Hand& h : s.hands) {
                out += std::format("{}{} ({}){}", out.empty() ? "" : " | ", hand_text(h.cards), bj::describe(h.cards),
                                   h.doubled ? " doubled" : "");
            }
            return out;
        }

        // The end of a round: everybody's results, and our winnings.
        void finish() {
            std::string results;
            for (const bj::Seat& s : table_.seats) {
                long long back = 0;
                std::string hands;
                for (const bj::Hand& h : s.hands) {
                    const long long bet = h.doubled ? 2 * s.bet : s.bet;
                    back += bj::payout(bet, bj::outcome(h.cards, s.hands.size() > 1, table_.dealer));
                    hands += std::format("{}{}", hands.empty() ? "" : " and ", bj::describe(h.cards));
                }
                results += std::format("{}{} ({}) {}", results.empty() ? "" : " · ", who(s.id, results.empty()), hands,
                                       net_text(back - s.staked(), s.staked(), s.id == chat_.id()));
                if (s.id == chat_.id()) {
                    own_back_ = back;
                }
            }
            chat_.notice(std::format("🃏 The dealer has {} ({}). {}", hand_text(table_.dealer),
                                     bj::describe(table_.dealer), results));
            settle();
        }

        // Pays us what the table gives back, and whatever we staked that the dealer did not take.
        void settle() {
            if (staked_ <= 0) {
                return;
            }
            const bj::Seat* mine = own_seat();
            const long long on_table = mine ? mine->staked() : 0;
            const long long back = (mine ? own_back_ : 0) + std::max(0LL, staked_ - on_table);
            games_.cash(back);
            if (!mine) {
                chat_.notice(std::format("🃏 Your bet did not reach the dealer in time: your {} are back.",
                                         plural(staked_, "coin")));
            } else {
                chat_.notice(std::format("💰 You {} at blackjack.", net_text(back - staked_, staked_)));
            }
            staked_ = 0;
            own_back_ = 0;
        }

        // A round given up: our bets back.
        void give_back() {
            if (staked_ > 0 && live()) {
                games_.cash(staked_);
                chat_.notice(std::format("🃏 Your {} at blackjack are back.", plural(staked_, "coin")));
            }
            staked_ = 0;
            pending_.reset();
        }

        // The table, for the casino's window.
        void publish() {
            shown_left_ = left();
            const std::string_view phase = round_ == 0           ? "idle"
                                           : table_.phase == 'b' ? "betting"
                                           : table_.phase == 'p' ? "playing"
                                                                 : "over";
            const auto cards_json = [](const std::vector<Card>& cards) {
                std::string out = "[";
                for (const Card c : cards) {
                    out += std::format("{}\"{}\"", out.size() > 1 ? "," : "", casino::card_code(c));
                }
                return out + "]";
            };
            std::string seats = "[";
            for (const bj::Seat& s : table_.seats) {
                std::string hands = "[";
                for (const bj::Hand& h : s.hands) {
                    const long long bet = h.doubled ? 2 * s.bet : s.bet;
                    std::string result = "null";
                    long long back = 0;
                    if (table_.phase == 'o') {
                        const auto o = bj::outcome(h.cards, s.hands.size() > 1, table_.dealer);
                        back = bj::payout(bet, o);
                        result = bj::total(h.cards).value > 21    ? "\"bust\""
                                 : o == bj::Outcome::Blackjack    ? "\"blackjack\""
                                 : o == bj::Outcome::Win          ? "\"win\""
                                 : o == bj::Outcome::Push         ? "\"push\""
                                                                  : "\"lose\"";
                    }
                    hands += std::format("{}{{\"cards\":{},\"total\":{},\"bet\":{},\"doubled\":{},\"done\":{},"
                                         "\"result\":{},\"back\":{}}}",
                                         hands.size() > 1 ? "," : "", cards_json(h.cards),
                                         json(bj::describe(h.cards)), bet, h.doubled, h.done, result, back);
                }
                hands += "]";
                seats += std::format("{}{{{},\"bet\":{},\"ready\":{},\"hands\":{}}}", seats.size() > 1 ? "," : "",
                                     player_json(s.id), s.bet, s.ready, hands);
            }
            seats += "]";
            const bj::Seat* mine = own_seat();
            const auto hand = own_hand();
            const bj::Hand* h = hand ? &mine->hands[*hand] : nullptr;
            const bool open = !live() || (table_.phase == 'b' && !mine && table_.seats.size() < bj::seats);
            const std::string state = std::format(
                "{{\"round\":\"{:x}\",\"phase\":\"{}\",\"left\":{},\"dealer\":{},\"youDeal\":{},\"dealerCards\":{},"
                "\"dealerTotal\":{},\"seats\":{},\"seated\":{},\"hand\":{},\"pending\":{},\"can\":{{\"bet\":{},"
                "\"deal\":{},\"hit\":{},\"stand\":{},\"double\":{},\"split\":{}}},\"min\":{},\"max\":{},"
                "\"betTime\":{},\"playTime\":{}}}",
                round_, phase, live() ? left() : 0, json(round_ ? name_of(dealer_) : ""), dealing_,
                cards_json(table_.dealer), json(table_.dealer.empty() ? "" : bj::describe(table_.dealer)), seats,
                mine != nullptr, hand ? static_cast<long long>(*hand) : -1, pending_.has_value(), open && !pending_,
                live() && table_.phase == 'b' && mine && !mine->ready, h != nullptr, h != nullptr,
                h && h->cards.size() == 2 && !h->doubled,
                h && mine->hands.size() == 1 && bj::can_split(h->cards), casino::min_bet, casino::max_bet,
                bet_time.count(), play_time.count());
            terminal_.show_game(game_name, state);
        }

        std::uint64_t round_ = 0;
        // Who deals the round, and whether it is us.
        std::uint64_t dealer_ = 0;
        bool dealing_ = false;
        bj::Table table_;
        // When the phase ends (the dealer's, or as last told), and when the dealer was last heard.
        clock::time_point deadline_;
        clock::time_point heard_at_;
        // The dealer's: the cards to deal, the face down one, when to tell the table next, and how many more times
        // the end is told.
        std::vector<Card> shoe_;
        Card hole_ = casino::hidden;
        clock::time_point next_send_;
        int finals_ = 0;
        // Ours: the coins on this round, what the end gives back, and a packet not seen at the table yet.
        long long staked_ = 0;
        long long own_back_ = 0;
        std::optional<Pending> pending_;
        long long shown_left_ = -1;
    };

} // namespace

std::unique_ptr<Game> make_blackjack(Chat& chat, Screen& terminal, Games& games) {
    return std::make_unique<Blackjack>(chat, terminal, games);
}

} // namespace zchat::game
