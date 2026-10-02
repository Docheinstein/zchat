// Tests of the casino's arithmetic (casino.hpp): cards, blackjack, roulette and the horse race.

#include "check.hpp"

#include "casino.hpp"

#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace zchat::casino;

namespace {

std::vector<Card> cards(std::string_view codes) {
    std::vector<Card> out;
    for (std::size_t i = 0; i + 1 < codes.size(); i += 2) {
        out.push_back(parse_card(codes.substr(i, 2)).value_or(hidden));
    }
    return out;
}

} // namespace

TEST(casino_cards) {
    for (Card c = 0; c <= hidden; ++c) {
        CHECK_EQ(parse_card(card_code(c)).value_or(99), c);
    }
    CHECK_EQ(card_code(parse_card("TD").value_or(0)), "TD");
    CHECK_EQ(card_label(parse_card("TD").value_or(0)), "10♦");
    CHECK_EQ(card_label(parse_card("AS").value_or(0)), "A♠");
    CHECK(!parse_card("1S").has_value());
    CHECK(!parse_card("A").has_value());
    std::mt19937_64 rng(7);
    auto s = shoe(2, rng);
    CHECK_EQ(s.size(), std::size_t {104});
    std::ranges::sort(s);
    CHECK(s.front() == 0 && s[1] == 0 && s.back() == 51);
}

TEST(casino_blackjack_totals) {
    using namespace blackjack;
    CHECK_EQ(total(cards("AS6H")).value, 17);
    CHECK(total(cards("AS6H")).soft);
    CHECK_EQ(total(cards("AS6HTD")).value, 17);
    CHECK(!total(cards("AS6HTD")).soft);
    CHECK_EQ(total(cards("ASAD")).value, 12);
    CHECK_EQ(total(cards("KSXX")).value, 10);
    CHECK(natural(cards("ASKD")));
    CHECK(!natural(cards("AS5D5C")));
    CHECK_EQ(describe(cards("ASKD")), "blackjack");
    CHECK_EQ(describe(cards("AS6H")), "soft 17");
    CHECK_EQ(describe(cards("KSQSTS")), "bust (30)");
    // The dealer stands on all 17s.
    CHECK(dealer_draws(cards("TS6H")));
    CHECK(!dealer_draws(cards("AS6H")));
    CHECK(!dealer_draws(cards("TS7H")));
    CHECK(can_split(cards("8S8D")));
    CHECK(can_split(cards("KSTD")));
    CHECK(!can_split(cards("8S9D")));
    CHECK(!can_split(cards("8S8D8C")));
}

TEST(casino_blackjack_outcomes) {
    using namespace blackjack;
    CHECK(outcome(cards("TS9H"), false, cards("TD8C")) == Outcome::Win);
    CHECK(outcome(cards("TS8H"), false, cards("TD8C")) == Outcome::Push);
    CHECK(outcome(cards("TS7H"), false, cards("TD8C")) == Outcome::Lose);
    CHECK(outcome(cards("TS7H"), false, cards("TD6C9S")) == Outcome::Win);
    // Busted, the player loses even when the dealer busts too.
    CHECK(outcome(cards("TS7H9C"), false, cards("TD6C9S")) == Outcome::Lose);
    CHECK(outcome(cards("ASKH"), false, cards("TD9C")) == Outcome::Blackjack);
    CHECK(outcome(cards("ASKH"), false, cards("ADKC")) == Outcome::Push);
    // 21 after a split is not a blackjack.
    CHECK(outcome(cards("ASKH"), true, cards("TD9C")) == Outcome::Win);
    // The dealer's blackjack beats any 21 of more cards.
    CHECK(outcome(cards("7S7H7C"), false, cards("ADKC")) == Outcome::Lose);
    CHECK_EQ(payout(10, Outcome::Lose), 0LL);
    CHECK_EQ(payout(10, Outcome::Push), 10LL);
    CHECK_EQ(payout(10, Outcome::Win), 20LL);
    CHECK_EQ(payout(10, Outcome::Blackjack), 25LL);
    CHECK_EQ(payout(5, Outcome::Blackjack), 12LL);
}

TEST(casino_blackjack_table) {
    using namespace blackjack;
    Table t;
    t.phase = 'p';
    t.left = 27;
    t.dealer = cards("KHXX");
    t.seats.push_back({0xabc, 10, true, {{cards("8S3H"), true, true}, {cards("8DKC"), false, false}}});
    t.seats.push_back({0x1234567890abcdef, 500, false, {}});
    const std::string text = encode(t);
    CHECK_EQ(text, "p 27 KHXX abc:10:1:8S3Hdx/8DKC 1234567890abcdef:500:0:-");
    const auto back = decode(text);
    REQUIRE(back.has_value());
    CHECK_EQ(encode(*back), text);
    CHECK_EQ(back->seats[0].hands.size(), std::size_t {2});
    CHECK(back->seats[0].hands[0].doubled && back->seats[0].hands[0].done);
    CHECK_EQ(back->seats[0].staked(), 30LL);
    CHECK_EQ(back->seats[1].staked(), 500LL);
    CHECK(back->dealer[1] == hidden);
    // Sender ids use all their 64 bits.
    const auto high = decode("b 15 - f234567890abcdef:10:0:-");
    REQUIRE(high.has_value());
    CHECK(high->seats[0].id == 0xf234567890abcdefULL);
    CHECK(decode("b 15 -").has_value());
    CHECK(!decode("z 15 -").has_value());
    CHECK(!decode("b 15 - abc:0:0:-").has_value());
    CHECK(!decode("b 15 - abc:10:2:-").has_value());
    CHECK(!decode("b 15 - abc:10:1:ZZ").has_value());
}

TEST(casino_roulette) {
    using namespace roulette;
    CHECK(red(1) && red(36) && !red(2) && !red(0));
    CHECK_EQ(parse("17").value_or(""), "17");
    CHECK_EQ(parse("0").value_or(""), "0");
    CHECK(!parse("37").has_value());
    CHECK(!parse("007").has_value());
    CHECK_EQ(parse("1st").value_or(""), "dozen1");
    CHECK_EQ(parse("col3").value_or(""), "column3");
    CHECK_EQ(parse("19-36").value_or(""), "high");
    CHECK(!parse("purple").has_value());
    CHECK_EQ(describe("dozen2"), "2nd dozen");
    CHECK_EQ(describe("column1"), "column 1");
    CHECK_EQ(payout("17", 10, 17), 360LL);
    CHECK_EQ(payout("17", 10, 18), 0LL);
    CHECK_EQ(payout("0", 10, 0), 360LL);
    CHECK_EQ(payout("red", 10, 1), 20LL);
    CHECK_EQ(payout("black", 10, 1), 0LL);
    CHECK_EQ(payout("red", 10, 0), 0LL);
    CHECK_EQ(payout("even", 10, 0), 0LL);
    CHECK_EQ(payout("odd", 10, 35), 20LL);
    CHECK_EQ(payout("low", 10, 18), 20LL);
    CHECK_EQ(payout("high", 10, 18), 0LL);
    CHECK_EQ(payout("dozen3", 10, 25), 30LL);
    CHECK_EQ(payout("dozen3", 10, 24), 0LL);
    CHECK_EQ(payout("column1", 10, 34), 30LL);
    CHECK_EQ(payout("column3", 10, 36), 30LL);
    CHECK_EQ(payout("column3", 10, 35), 0LL);
    // Every bet loses a little, on average: 36 out of 37 comes back.
    for (const std::string_view bet : {"7", "red", "odd", "high", "dozen2", "column2"}) {
        long long back = 0;
        for (int n = 0; n < pockets; ++n) {
            back += payout(bet, 1, n);
        }
        CHECK_EQ(back, 36LL);
    }
}

TEST(casino_horses) {
    using namespace horses;
    std::mt19937_64 rng(42);
    for (int race = 0; race < 300; ++race) {
        const Field f = field(rng);
        REQUIRE(f.horses.size() == static_cast<std::size_t>(runners));
        for (const Horse& h : f.horses) {
            CHECK(h.odds >= 11);
        }
        const auto decoded = decode_field(encode_field(f.horses));
        REQUIRE(decoded.has_value());
        CHECK_EQ(encode_field(*decoded), encode_field(f.horses));

        const auto order = finish_order(f.weights, rng);
        auto sorted = order;
        std::ranges::sort(sorted);
        CHECK(sorted == std::vector<int>({0, 1, 2, 3, 4, 5}));
        // The track has them finish in that order, and everybody finishes.
        const Track t = track(order, rng);
        for (std::size_t place = 1; place < order.size(); ++place) {
            const double before = finish_time(t[static_cast<std::size_t>(order[place - 1])]);
            const double after = finish_time(t[static_cast<std::size_t>(order[place])]);
            CHECK(before < after);
            CHECK(after <= checkpoints);
        }
        const auto back = decode_race(encode_race(order, t), order.size());
        REQUIRE(back.has_value());
        CHECK(back->order == order);
        CHECK(back->track == t);
    }
    CHECK_EQ(payout(10, 34), 34LL);
    CHECK_EQ(payout(3, 25), 7LL);
    CHECK_EQ(odds_text(34), "3.4");
    CHECK(!decode_field("1:5").has_value());
    CHECK(!decode_race("01 00", 2).has_value());
}

TEST(casino_horses_odds_are_fair) {
    // The horses win about as often as their weights say: the favourite of a field more than the outsider.
    using namespace horses;
    std::mt19937_64 rng(1);
    const std::vector<double> weights {5, 1, 1, 1, 1, 1};
    std::map<int, int> wins;
    for (int i = 0; i < 10000; ++i) {
        ++wins[finish_order(weights, rng).front()];
    }
    CHECK(wins[0] > 4500 && wins[0] < 5500);
    CHECK(wins[1] > 700 && wins[1] < 1300);
}
