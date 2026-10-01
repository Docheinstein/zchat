// Tests of the Elo ratings of the games (elo.hpp).

#include "check.hpp"

#include "elo.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace zchat::elo;

namespace {

bool near(double a, double b) {
    return std::abs(a - b) < 0.01;
}

} // namespace

TEST(elo_expected) {
    CHECK(near(expected(1500, 1500), 0.5));
    CHECK(near(expected(1900, 1500), 1 / (1 + std::pow(10.0, -1.0))));
    CHECK(near(expected(1500, 1700) + expected(1700, 1500), 1));
}

TEST(elo_two_players) {
    // Equal and new: the winner gets half of K.
    auto d = deltas({{}, {}}, {0, 1});
    CHECK(near(d[0], 20));
    CHECK(near(d[1], -20));
    // Settled ratings move less.
    d = deltas({{1500, 30}, {1500, 30}}, {1, 0});
    CHECK(near(d[0], -10));
    CHECK(near(d[1], 10));
    // A tie between equals changes nothing; between unequal ones, the weaker gains.
    d = deltas({{}, {}}, {0, 0});
    CHECK(near(d[0], 0));
    d = deltas({{1400, 30}, {1600, 30}}, {0, 0});
    CHECK(d[0] > 0);
    CHECK(near(d[0], -d[1]));
    // Beating a much weaker player gives little.
    d = deltas({{1900, 30}, {1500, 30}}, {0, 1});
    CHECK(d[0] > 0 && d[0] < 3);
}

TEST(elo_many_players) {
    // Equal and settled: first and last move by the same amount, the middle one not at all.
    const std::vector<Rating> three(3, Rating {1500, 30});
    auto d = deltas(three, {0, 1, 2});
    CHECK(near(d[0], 10));
    CHECK(near(d[1], 0));
    CHECK(near(d[2], -10));
    // One winner and the rest tied last.
    d = deltas(three, {0, 1, 1});
    CHECK(near(d[0], 10));
    CHECK(near(d[1], -5));
    CHECK(near(d[2], -5));
    // Alone, nothing.
    CHECK(near(deltas({{}}, {0})[0], 0));
}

TEST(elo_encode_decode) {
    Ratings r;
    r["general"] = {1512.44, 12};
    r["race"] = {1530, 5};
    const std::string text = encode(r);
    CHECK_EQ(text, std::string("general 1512.4 12 race 1530.0 5"));
    const auto back = decode(text);
    REQUIRE(back.has_value());
    CHECK_EQ(back->size(), std::size_t(2));
    CHECK(near(back->at("general").rating, 1512.4));
    CHECK_EQ(back->at("race").games, 5);
    CHECK(decode("")->empty());
    CHECK(!decode("race 1500").has_value());
    CHECK(!decode("race x 1").has_value());
    CHECK(!decode("Race 1500 1").has_value());
    CHECK(!decode("race 99999 1").has_value());
    CHECK(!decode("race 1500 -1").has_value());
    CHECK(!decode("race\x1b 1500 1").has_value());
}
