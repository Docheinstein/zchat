#include "casino.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <numeric>

namespace zchat::casino {

namespace {

    constexpr std::string_view ranks = "A23456789TJQK";
    constexpr std::string_view suits = "SHDC";

    std::optional<long long> parse_number(std::string_view s, int base = 10) {
        long long value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
        if (s.empty() || s.front() == '-' || ec != std::errc {} || ptr != s.data() + s.size()) {
            return std::nullopt;
        }
        return value;
    }

    // Takes the next field of s, up to sep, and removes it with sep.
    std::string_view next_field(std::string_view& s, char sep = ' ') {
        const auto at = s.find(sep);
        const std::string_view field = s.substr(0, at);
        s.remove_prefix(at == std::string_view::npos ? s.size() : at + 1);
        return field;
    }

    int hex_digit(char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    }

} // namespace

std::string card_code(Card card) {
    if (card >= hidden) {
        return "XX";
    }
    return {ranks[card % 13], suits[card / 13]};
}

std::optional<Card> parse_card(std::string_view code) {
    if (code == "XX") {
        return hidden;
    }
    if (code.size() != 2) {
        return std::nullopt;
    }
    const auto rank = ranks.find(code[0]);
    const auto suit = suits.find(code[1]);
    if (rank == std::string_view::npos || suit == std::string_view::npos) {
        return std::nullopt;
    }
    return static_cast<Card>(suit * 13 + rank);
}

std::string card_label(Card card) {
    if (card >= hidden) {
        return "🂠";
    }
    static constexpr std::array<std::string_view, 13> rank_labels {"A", "2", "3",  "4", "5", "6", "7",
                                                                   "8", "9", "10", "J", "Q", "K"};
    static constexpr std::array<std::string_view, 4> suit_labels {"♠", "♥", "♦", "♣"};
    return std::format("{}{}", rank_labels[card % 13], suit_labels[card / 13]);
}

std::vector<Card> shoe(int decks, std::mt19937_64& rng) {
    std::vector<Card> cards;
    for (int d = 0; d < decks; ++d) {
        for (Card c = 0; c < hidden; ++c) {
            cards.push_back(c);
        }
    }
    std::ranges::shuffle(cards, rng);
    return cards;
}

namespace blackjack {

    namespace {

        // A card's points, the ace as 1.
        int points(Card card) {
            return std::min(10, card % 13 + 1);
        }

        std::string encode_hand(const Hand& hand) {
            std::string out;
            for (const Card c : hand.cards) {
                out += card_code(c);
            }
            if (hand.doubled) {
                out += 'd';
            }
            if (hand.done) {
                out += 'x';
            }
            return out.empty() ? "-" : out;
        }

        std::optional<Hand> decode_hand(std::string_view text) {
            Hand hand;
            if (text == "-") {
                return hand;
            }
            while (!text.empty() && (text.back() == 'd' || text.back() == 'x')) {
                (text.back() == 'd' ? hand.doubled : hand.done) = true;
                text.remove_suffix(1);
            }
            if (text.size() % 2 != 0 || text.size() > 2 * 22) {
                return std::nullopt;
            }
            for (std::size_t i = 0; i < text.size(); i += 2) {
                const auto card = parse_card(text.substr(i, 2));
                if (!card) {
                    return std::nullopt;
                }
                hand.cards.push_back(*card);
            }
            return hand;
        }

    } // namespace

    Total total(const std::vector<Card>& cards) {
        int value = 0;
        bool ace = false;
        for (const Card c : cards) {
            if (c < hidden) {
                value += points(c);
                ace = ace || c % 13 == 0;
            }
        }
        if (ace && value + 10 <= 21) {
            return {value + 10, true};
        }
        return {value, false};
    }

    std::string describe(const std::vector<Card>& cards) {
        if (natural(cards)) {
            return "blackjack";
        }
        const Total t = total(cards);
        if (t.value > 21) {
            return std::format("bust ({})", t.value);
        }
        return t.soft ? std::format("soft {}", t.value) : std::to_string(t.value);
    }

    bool natural(const std::vector<Card>& cards) {
        return cards.size() == 2 && total(cards).value == 21;
    }

    bool dealer_draws(const std::vector<Card>& cards) {
        return total(cards).value < 17;
    }

    bool can_split(const std::vector<Card>& cards) {
        return cards.size() == 2 && cards[0] < hidden && cards[1] < hidden && points(cards[0]) == points(cards[1]);
    }

    Outcome outcome(const std::vector<Card>& hand, bool split, const std::vector<Card>& dealer) {
        const int mine = total(hand).value;
        if (mine > 21) {
            return Outcome::Lose;
        }
        const bool dealer_natural = natural(dealer);
        if (!split && natural(hand)) {
            return dealer_natural ? Outcome::Push : Outcome::Blackjack;
        }
        if (dealer_natural) {
            return Outcome::Lose;
        }
        const int theirs = total(dealer).value;
        if (theirs > 21 || mine > theirs) {
            return Outcome::Win;
        }
        return mine == theirs ? Outcome::Push : Outcome::Lose;
    }

    long long payout(long long bet, Outcome outcome) {
        switch (outcome) {
        case Outcome::Lose:
            return 0;
        case Outcome::Push:
            return bet;
        case Outcome::Win:
            return 2 * bet;
        case Outcome::Blackjack:
            return bet + bet * 3 / 2;
        }
        return 0;
    }

    long long Seat::staked() const {
        if (hands.empty()) {
            return bet;
        }
        long long sum = 0;
        for (const Hand& h : hands) {
            sum += h.doubled ? 2 * bet : bet;
        }
        return sum;
    }

    std::string encode(const Table& table) {
        std::string dealer;
        for (const Card c : table.dealer) {
            dealer += card_code(c);
        }
        std::string out = std::format("{} {} {}", table.phase, table.left, dealer.empty() ? "-" : dealer);
        for (const Seat& s : table.seats) {
            std::string hands;
            for (const Hand& h : s.hands) {
                hands += (hands.empty() ? "" : "/") + encode_hand(h);
            }
            out += std::format(" {:x}:{}:{}:{}", s.id, s.bet, s.ready ? 1 : 0, hands.empty() ? "-" : hands);
        }
        return out;
    }

    std::optional<Table> decode(std::string_view text) {
        Table table;
        const std::string_view phase = next_field(text);
        if (phase != "b" && phase != "p" && phase != "o") {
            return std::nullopt;
        }
        table.phase = phase[0];
        const auto left = parse_number(next_field(text));
        if (!left || *left > 3600) {
            return std::nullopt;
        }
        table.left = static_cast<int>(*left);
        const std::string_view dealer = next_field(text);
        if (dealer != "-") {
            const auto hand = decode_hand(dealer);
            if (!hand || hand->doubled || hand->done) {
                return std::nullopt;
            }
            table.dealer = hand->cards;
        }
        while (!text.empty()) {
            std::string_view seat = next_field(text);
            Seat s;
            // Sender ids take all 64 bits.
            const std::string_view id_text = next_field(seat, ':');
            std::uint64_t id_value = 0;
            const auto [id_end, id_error] =
                std::from_chars(id_text.data(), id_text.data() + id_text.size(), id_value, 16);
            const auto id = id_error == std::errc {} && id_end == id_text.data() + id_text.size() && !id_text.empty()
                                ? std::optional(id_value)
                                : std::nullopt;
            const auto bet = parse_number(next_field(seat, ':'));
            const std::string_view ready = next_field(seat, ':');
            if (!id || *id == 0 || !bet || *bet < min_bet || *bet > max_bet || (ready != "0" && ready != "1") ||
                table.seats.size() >= seats) {
                return std::nullopt;
            }
            s.id = *id;
            s.bet = *bet;
            s.ready = ready == "1";
            if (seat != "-") {
                while (!seat.empty()) {
                    const auto hand = decode_hand(next_field(seat, '/'));
                    if (!hand || s.hands.size() >= 2) {
                        return std::nullopt;
                    }
                    s.hands.push_back(*hand);
                }
            }
            table.seats.push_back(std::move(s));
        }
        return table;
    }

} // namespace blackjack

namespace roulette {

    bool red(int number) {
        static constexpr std::array reds {1, 3, 5, 7, 9, 12, 14, 16, 18, 19, 21, 23, 25, 27, 30, 32, 34, 36};
        return std::ranges::find(reds, number) != reds.end();
    }

    std::optional<std::string> parse(std::string_view typed) {
        if (const auto n = parse_number(typed); n && typed.size() <= 2 && *n < pockets) {
            return std::to_string(*n);
        }
        struct Alias {
            std::string_view typed;
            std::string_view bet;
        };
        static constexpr std::array aliases {
            Alias {"red", "red"},         Alias {"black", "black"},     Alias {"odd", "odd"},
            Alias {"even", "even"},       Alias {"low", "low"},         Alias {"1-18", "low"},
            Alias {"high", "high"},       Alias {"19-36", "high"},      Alias {"dozen1", "dozen1"},
            Alias {"1st", "dozen1"},      Alias {"1st12", "dozen1"},    Alias {"1-12", "dozen1"},
            Alias {"dozen2", "dozen2"},   Alias {"2nd", "dozen2"},      Alias {"2nd12", "dozen2"},
            Alias {"13-24", "dozen2"},    Alias {"dozen3", "dozen3"},   Alias {"3rd", "dozen3"},
            Alias {"3rd12", "dozen3"},    Alias {"25-36", "dozen3"},    Alias {"column1", "column1"},
            Alias {"col1", "column1"},    Alias {"column2", "column2"}, Alias {"col2", "column2"},
            Alias {"column3", "column3"}, Alias {"col3", "column3"},
        };
        const auto it = std::ranges::find(aliases, typed, &Alias::typed);
        return it == aliases.end() ? std::nullopt : std::optional(std::string(it->bet));
    }

    std::string describe(std::string_view bet) {
        if (bet.starts_with("dozen") && bet.size() == 6) {
            static constexpr std::array<std::string_view, 3> which {"1st", "2nd", "3rd"};
            return std::format("{} dozen", which[static_cast<std::size_t>(bet[5] - '1') % 3]);
        }
        if (bet.starts_with("column") && bet.size() == 7) {
            return std::format("column {}", bet[6]);
        }
        if (bet == "low") {
            return "1 to 18";
        }
        if (bet == "high") {
            return "19 to 36";
        }
        return std::string(bet);
    }

    long long payout(std::string_view bet, long long amount, int number) {
        if (const auto n = parse_number(bet)) {
            return *n == number ? 36 * amount : 0;
        }
        if (number <= 0 || number >= pockets) {
            return 0;
        }
        bool won = false;
        long long times = 2;
        if (bet == "red" || bet == "black") {
            won = red(number) == (bet == "red");
        } else if (bet == "odd" || bet == "even") {
            won = (number % 2 == 1) == (bet == "odd");
        } else if (bet == "low" || bet == "high") {
            won = (number <= 18) == (bet == "low");
        } else if (bet.starts_with("dozen") && bet.size() == 6) {
            won = (number - 1) / 12 == bet[5] - '1';
            times = 3;
        } else if (bet.starts_with("column") && bet.size() == 7) {
            won = (number - 1) % 3 == bet[6] - '1';
            times = 3;
        }
        return won ? times * amount : 0;
    }

} // namespace roulette

namespace horses {

    namespace {

        // How much of what is bet the odds give back, on average.
        constexpr double returned = 0.9;

        // One of weights, by weight, of those not taken.
        std::size_t draw(const std::vector<double>& weights, const std::vector<bool>& taken, std::mt19937_64& rng) {
            double sum = 0;
            for (std::size_t i = 0; i < weights.size(); ++i) {
                sum += taken[i] ? 0 : weights[i];
            }
            double at = std::uniform_real_distribution<double>(0, sum)(rng);
            std::size_t last = weights.size();
            for (std::size_t i = 0; i < weights.size(); ++i) {
                if (taken[i]) {
                    continue;
                }
                last = i;
                if (at < weights[i]) {
                    return i;
                }
                at -= weights[i];
            }
            return last;
        }

    } // namespace

    Field field(std::mt19937_64& rng) {
        std::vector<int> all(names.size());
        std::iota(all.begin(), all.end(), 0);
        std::ranges::shuffle(all, rng);
        Field f;
        for (int i = 0; i < runners; ++i) {
            f.horses.push_back({all[static_cast<std::size_t>(i)], 0});
            f.weights.push_back(std::uniform_real_distribution<double>(1.0, 6.0)(rng));
        }
        const double sum = std::accumulate(f.weights.begin(), f.weights.end(), 0.0);
        for (std::size_t i = 0; i < f.horses.size(); ++i) {
            const double chance = f.weights[i] / sum;
            f.horses[i].odds = std::max(11, static_cast<int>(std::floor(returned / chance * 10)));
        }
        return f;
    }

    std::vector<int> finish_order(const std::vector<double>& weights, std::mt19937_64& rng) {
        std::vector<bool> taken(weights.size(), false);
        std::vector<int> order;
        for (std::size_t n = 0; n < weights.size(); ++n) {
            const std::size_t i = draw(weights, taken, rng);
            taken[i] = true;
            order.push_back(static_cast<int>(i));
        }
        return order;
    }

    Track track(const std::vector<int>& order, std::mt19937_64& rng) {
        Track out(order.size());
        std::uniform_real_distribution<double> uniform(0, 1);
        // When each finishes, in checkpoints: the winner a little before the end of the race, the others after it.
        double when = 14.0 + uniform(rng);
        for (std::size_t place = 0; place < order.size(); ++place) {
            if (place > 0) {
                when += 0.3 + 0.5 * uniform(rng);
            }
            // How fast it goes at each checkpoint: a bit at random, and faster at the start or at the end.
            const double style = uniform(rng) * 0.8 - 0.4;
            std::array<double, checkpoints + 1> run {};
            for (int k = 1; k <= checkpoints; ++k) {
                const double speed = (0.6 + 0.8 * uniform(rng)) * (1 + style * (k - checkpoints / 2.0) / checkpoints);
                run[static_cast<std::size_t>(k)] = run[static_cast<std::size_t>(k - 1)] + std::max(0.1, speed);
            }
            const auto whole = static_cast<std::size_t>(when);
            const double at_finish = run[whole] + (run[whole + 1] - run[whole]) * (when - static_cast<double>(whole));
            auto& positions = out[static_cast<std::size_t>(order[place])];
            for (int k = 1; k <= checkpoints; ++k) {
                const double at = finish * run[static_cast<std::size_t>(k)] / at_finish;
                positions[static_cast<std::size_t>(k - 1)] =
                    std::clamp(static_cast<int>(std::lround(at)), 0, k < when ? finish - 1 : 255);
            }
        }
        return out;
    }

    double finish_time(const std::array<int, checkpoints>& positions) {
        int before = 0;
        for (int k = 0; k < checkpoints; ++k) {
            const int at = positions[static_cast<std::size_t>(k)];
            if (at >= finish) {
                return k + static_cast<double>(finish - before) / std::max(1, at - before);
            }
            before = at;
        }
        return checkpoints + 1;
    }

    long long payout(long long amount, int odds) {
        return amount * odds / 10;
    }

    std::string odds_text(int odds) {
        return std::format("{}.{}", odds / 10, odds % 10);
    }

    std::string encode_field(const std::vector<Horse>& horses) {
        std::string out;
        for (const Horse& h : horses) {
            out += std::format("{}{}:{}", out.empty() ? "" : ",", h.name, h.odds);
        }
        return out;
    }

    std::optional<std::vector<Horse>> decode_field(std::string_view text) {
        std::vector<Horse> horses;
        while (!text.empty()) {
            std::string_view horse = next_field(text, ',');
            const auto name = parse_number(next_field(horse, ':'));
            const auto odds = parse_number(horse);
            if (!name || *name >= static_cast<long long>(names.size()) || !odds || *odds < 11 || *odds > 9999 ||
                horses.size() >= 10) {
                return std::nullopt;
            }
            horses.push_back({static_cast<int>(*name), static_cast<int>(*odds)});
        }
        if (horses.size() < 2) {
            return std::nullopt;
        }
        return horses;
    }

    std::string encode_race(const std::vector<int>& order, const Track& track) {
        std::string out;
        for (const int i : order) {
            out += static_cast<char>('0' + i);
        }
        out += ' ';
        for (const auto& positions : track) {
            for (const int at : positions) {
                out += std::format("{:02x}", at);
            }
        }
        return out;
    }

    std::optional<Race> decode_race(std::string_view text, std::size_t horses) {
        const std::string_view order = next_field(text);
        if (order.size() != horses || text.size() != horses * checkpoints * 2) {
            return std::nullopt;
        }
        Race race;
        for (const char c : order) {
            const int i = c - '0';
            if (i < 0 || i >= static_cast<int>(horses) || std::ranges::find(race.order, i) != race.order.end()) {
                return std::nullopt;
            }
            race.order.push_back(i);
        }
        race.track.resize(horses);
        for (std::size_t h = 0; h < horses; ++h) {
            for (std::size_t k = 0; k < checkpoints; ++k) {
                const std::size_t at = (h * checkpoints + k) * 2;
                const int hi = hex_digit(text[at]);
                const int lo = hex_digit(text[at + 1]);
                if (hi < 0 || lo < 0) {
                    return std::nullopt;
                }
                race.track[h][k] = hi * 16 + lo;
            }
        }
        return race;
    }

} // namespace horses

} // namespace zchat::casino
