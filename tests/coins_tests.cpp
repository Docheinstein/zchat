// Tests of the coins won in the games and spent on the annoying commands (coins.hpp).

#include "check.hpp"

#include "coins.hpp"

#include <string>
#include <vector>

using namespace zchat::coins;

TEST(coins_payouts) {
    // A duel: the winner gets more than the loser, who still gets something for playing.
    auto p = payouts({0, 1});
    CHECK_EQ(p[0], for_playing + per_beaten);
    CHECK_EQ(p[1], for_playing);
    // A tie: the same for both.
    p = payouts({0, 0});
    CHECK_EQ(p[0], for_playing + per_tie);
    CHECK_EQ(p[1], p[0]);
    // Five players: the winner of a big round gets the most.
    p = payouts({0, 1, 2, 3, 4});
    CHECK_EQ(p[0], for_playing + 4 * per_beaten);
    CHECK_EQ(p[2], for_playing + 2 * per_beaten);
    CHECK_EQ(p[4], for_playing);
    // One winner and the rest tied last.
    p = payouts({0, 1, 1});
    CHECK_EQ(p[0], for_playing + 2 * per_beaten);
    CHECK_EQ(p[1], for_playing + per_tie);
    // Alone, or nobody: nothing.
    CHECK_EQ(payouts({0})[0], 0LL);
    CHECK(payouts({}).empty());
}

TEST(coins_prices) {
    CHECK_EQ(price("trill").value_or(0), 5LL);
    CHECK(price("custom trill").value_or(0) > price("trill").value_or(0));
    CHECK_EQ(price("kick").value_or(0), 20LL);
    CHECK(price("spy").has_value());
    CHECK(!price("nothing").has_value());
    // A spy is real now (not "coming soon"): the asker pays 20 when accepted, the one agreeing gets half.
    CHECK_EQ(price("spy").value_or(0), 20LL);
    for (const Item& item : items) {
        if (item.name == "spy") {
            CHECK(!item.soon);
        }
    }
    // New people can afford to ask for a peek right away.
    CHECK(initial >= price("spy").value_or(0));
    // A duel won pays a trill.
    CHECK(payouts({0, 1})[0] >= price("trill").value_or(0));
    // New people can trill and kick right away.
    CHECK(initial >= price("kick").value_or(0) + price("custom trill").value_or(0));
}

TEST(coins_encode_decode) {
    CHECK_EQ(encode(56), std::string("56"));
    CHECK_EQ(encode(-3), std::string("0"));
    CHECK_EQ(encode(most + 1), encode(most));
    CHECK_EQ(decode("56").value_or(-1), 56LL);
    CHECK_EQ(decode("0").value_or(-1), 0LL);
    CHECK_EQ(decode(encode(most)).value_or(-1), most);
    CHECK(!decode("").has_value());
    CHECK(!decode("-5").has_value());
    CHECK(!decode("12a").has_value());
    CHECK(!decode(" 12").has_value());
    CHECK(!decode("99999999999").has_value());
}
