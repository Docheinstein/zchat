#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// The casino: blackjack, roulette and a horse race, played for coins (see coins.hpp) at tables the whole chat shares
// (see blackjack.cpp, roulette.cpp and horses.cpp). This is their arithmetic, which needs no chat: the cards and
// their totals, what each bet pays, the horses' odds and how they run, and how the tables are written in packets.
namespace zchat::casino {

// What one bet can be: from a coin to a few rounds' winnings.
inline constexpr long long min_bet = 1;
inline constexpr long long max_bet = 500;

// A card of a 52-card deck: rank (c % 13: 0 is the ace, 9 the ten, 12 the king) and suit (c / 13: spades, hearts,
// diamonds, clubs). hidden is the dealer's face down card, as the players see it.
using Card = std::uint8_t;
inline constexpr Card hidden = 52;

// As written in packets: rank and suit, like "AS", "TD", "9H", or "XX" for hidden.
std::string card_code(Card card);
std::optional<Card> parse_card(std::string_view code);
// As shown: "A♠", "10♥", or "🂠" for hidden.
std::string card_label(Card card);

// decks shuffled together.
std::vector<Card> shoe(int decks, std::mt19937_64& rng);

namespace blackjack {

    // Players at a table, like a real one.
    inline constexpr std::size_t seats = 7;
    inline constexpr int decks = 6;

    // A hand's points: aces count 11 when that does not bust it (a soft total), or else 1. Hidden cards do not
    // count.
    struct Total {
        int value = 0;
        bool soft = false;
    };
    Total total(const std::vector<Card>& cards);
    // "17", "soft 17", "21", "bust (24)"; "blackjack" for two cards making 21.
    std::string describe(const std::vector<Card>& cards);
    // Two cards making 21.
    bool natural(const std::vector<Card>& cards);
    // The dealer draws to 16 and stands on all 17s, soft ones too.
    bool dealer_draws(const std::vector<Card>& cards);
    // Two cards of the same value (a king and a ten too) can be split, once.
    bool can_split(const std::vector<Card>& cards);

    // How a hand ends against the dealer's, and what it gives back for its bet: nothing, the bet (a push), twice it,
    // or 3 to 2 for a blackjack (two cards making 21, not after a split). A busted hand loses, whatever the dealer has.
    enum class Outcome { Lose, Push, Win, Blackjack };
    Outcome outcome(const std::vector<Card>& hand, bool split, const std::vector<Card>& dealer);
    long long payout(long long bet, Outcome outcome);

    // A player's hand: its cards, whether it was doubled (twice the bet, one card more) and whether it is done.
    struct Hand {
        std::vector<Card> cards;
        bool doubled = false;
        bool done = false;
    };

    // A player at the table: their sender id, bet (of each hand), whether they want the cards dealt, and their hands
    // (two after a split, none before the deal).
    struct Seat {
        std::uint64_t id = 0;
        long long bet = 0;
        bool ready = false;
        std::vector<Hand> hands;

        // What they have on the table: each hand's bet, twice when doubled.
        long long staked() const;
    };

    // The table, as the dealer tells it: taking bets ('b'), being played ('p'), or over ('o'), the seconds left of
    // it, the dealer's cards (the second one hidden until the dealer plays), and the players.
    struct Table {
        char phase = 'b';
        int left = 0;
        std::vector<Card> dealer;
        std::vector<Seat> seats;
    };

    // As written in a packet: "PHASE LEFT DEALER SEAT...", the dealer's cards one after the other ("KHXX"), and each
    // seat "ID:BET:READY:HAND[/HAND]", the id in hex, each hand its cards and then 'd' when doubled and 'x' when done,
    // or "-" for none. Read back, nullopt when it is not one.
    std::string encode(const Table& table);
    std::optional<Table> decode(std::string_view text);

} // namespace blackjack

namespace roulette {

    // A European wheel: 0 to 36.
    inline constexpr int pockets = 37;
    // How many bets one player can have on a spin.
    inline constexpr std::size_t most_bets = 30;

    bool red(int number);

    // A bet as typed, made into its name: a number ("17"), red, black, odd, even, low (1 to 18), high (19 to 36),
    // dozen1 to dozen3 (1st, 2nd and 3rd 12 too) or column1 to column3 (col1...); nullopt when it is not one.
    std::optional<std::string> parse(std::string_view typed);
    // As shown: "17", "red", "1st dozen", "column 2".
    std::string describe(std::string_view bet);
    // What a bet of amount on bet gives back when the ball stops on number: 36 times it for a number, 3 times for a
    // dozen or a column, twice for the rest; nothing when it loses. Zero loses them all but a bet on 0.
    long long payout(std::string_view bet, long long amount, int number);

} // namespace roulette

namespace horses {

    // Horses in a race, and the points of the race told (where each horse is, from 0 to finish).
    inline constexpr int runners = 6;
    inline constexpr int checkpoints = 20;
    inline constexpr int finish = 100;

    inline constexpr std::array names {
        "Thunder Hoof",  "Lucky Clover",  "Midnight Express", "Silver Bullet", "Pixel Pony",   "Turbo Oats",
        "Golden Mane",   "Rusty Spur",    "Stack Overflow",   "Null Pointer",  "Segfault Sue", "Lazy Susan",
        "Velvet Comet",  "Biscuit",       "Dark Horse",       "Hay Fever",     "Neigh Sayer",  "Gallop Poll",
        "Mane Event",    "Seabiscuit Jr", "Rapid Rhubarb",    "Wild Card",     "Photo Finish", "Last Minute",
    };

    // A horse of a race: its name (of names) and the odds it pays, in tenths: 34 gives back 3.4 times the bet.
    struct Horse {
        int name = 0;
        int odds = 20;
    };

    // The field of a race: the horses, and how likely each is to win (their weights). The odds give back about 90% of
    // what is bet on average: the house wins.
    struct Field {
        std::vector<Horse> horses;
        std::vector<double> weights;
    };
    Field field(std::mt19937_64& rng);

    // Where the horses finish, the winner first: each place is drawn by the weights of the horses left.
    std::vector<int> finish_order(const std::vector<double>& weights, std::mt19937_64& rng);

    // How the race goes, so that the horses finish in order: for each horse, where it is at each of the checkpoints
    // (0 to finish). Between them, a horse runs straight, so the first to reach finish is the winner, then the
    // second...
    using Track = std::vector<std::array<int, checkpoints>>;
    Track track(const std::vector<int>& order, std::mt19937_64& rng);

    // When a horse finishes, in checkpoints (fractional), going straight between them; checkpoints + 1 if it does not.
    double finish_time(const std::array<int, checkpoints>& positions);

    // What a winning bet of amount gives back.
    long long payout(long long amount, int odds);

    // "2.5", for 25.
    std::string odds_text(int odds);

    // As written in packets: the field "NAME:ODDS,NAME:ODDS..." and the race "ORDER TRACK", the order of finish as
    // digits ("305142") and the track as two hex digits per position, horse after horse. Read back, nullopt when they
    // are not ones.
    std::string encode_field(const std::vector<Horse>& horses);
    std::optional<std::vector<Horse>> decode_field(std::string_view text);
    std::string encode_race(const std::vector<int>& order, const Track& track);
    struct Race {
        std::vector<int> order;
        Track track;
    };
    std::optional<Race> decode_race(std::string_view text, std::size_t horses);

} // namespace horses

} // namespace zchat::casino
