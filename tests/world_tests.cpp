// Tests of where people are in the world, and what shows over their heads (world_place.hpp).

#include "check.hpp"

#include "markup.hpp"
#include "world_place.hpp"

#include <string>

using namespace zchat::world;

TEST(world_place_round_trip) {
    const Place place {"lobby", 250, 640, true, false};
    CHECK_EQ(encode_place(place), std::string("lobby 250 640 1 0"));
    const auto back = parse_place(encode_place(place));
    REQUIRE(back.has_value());
    CHECK(*back == place);
    const auto walking = parse_place("lobby 0 6400 0 1");
    REQUIRE(walking.has_value());
    CHECK(walking->walking);
    CHECK(!walking->left);
    CHECK_EQ(walking->y, 6400);
}

TEST(world_place_rejects_garbage) {
    CHECK(!parse_place("").has_value());
    CHECK(!parse_place("lobby 1 2 0").has_value());
    CHECK(!parse_place("lobby 1 2 0 1 extra").has_value());
    CHECK(!parse_place("lobby 1 2 0 2").has_value());
    CHECK(!parse_place("lobby -1 2 0 0").has_value());
    CHECK(!parse_place("lobby 1 6401 0 0").has_value());
    CHECK(!parse_place("lobby 1.5 2 0 0").has_value());
    CHECK(!parse_place("Lobby 1 2 0 0").has_value());
    CHECK(!parse_place("lob<by 1 2 0 0").has_value());
    CHECK(!parse_place("lobby  1 2 0 0").has_value());
}

TEST(world_bubble_plain_text) {
    CHECK_EQ(bubble_text("hello <color=red>world</color>!"), std::string("hello world!"));
    CHECK_EQ(bubble_text("  lots   of\tspace  "), std::string("lots of space"));
    CHECK_EQ(bubble_text("<b></b>"), std::string());
}

TEST(world_bubble_quote_shows_the_reply) {
    const std::string message = zchat::markup::quote("Rex", "lunch?") + "always 🦖";
    CHECK_EQ(bubble_text(message), std::string("always 🦖"));
}

TEST(world_bubble_cut_when_long) {
    CHECK_EQ(bubble_text("abcdefghij", 10), std::string("abcdefghij"));
    CHECK_EQ(bubble_text("abcdefghijk", 10), std::string("abcdefghij…"));
    // Characters, not bytes: an emoji is one.
    CHECK_EQ(bubble_text("🦖🦖🦖", 2), std::string("🦖🦖…"));
    CHECK_EQ(bubble_text("ab cd", 3), std::string("ab…"));
}
