// Tests of the quotes of messages (markup.hpp).

#include "check.hpp"

#include "markup.hpp"

#include <string>

using namespace zchat::markup;

TEST(markup_quote_round_trip) {
    const std::string message = quote("Rex", "anyone up for <b>lunch</b>?") + "always";
    CHECK_EQ(message, std::string("<quote=Rex>anyone up for lunch?</quote> always"));
    const auto q = split_quote(message);
    REQUIRE(q.has_value());
    CHECK_EQ(std::string(q->name), std::string("Rex"));
    CHECK_EQ(std::string(q->text), std::string("anyone up for lunch?"));
    CHECK_EQ(std::string(q->reply), std::string("always"));
}

TEST(markup_quote_not_a_quote) {
    CHECK(!split_quote("hello").has_value());
    CHECK(!split_quote("<quote=Rex>never closed").has_value());
    CHECK(!split_quote("<quote=>empty name</quote> hi").has_value());
    CHECK(!split_quote("<quote=Rex></quote> nothing quoted").has_value());
    CHECK(!split_quote("hi <quote=Rex>not at the start</quote>").has_value());
}

TEST(markup_quote_of_a_quote) {
    // Quoting a reply quotes the reply only, not what it quoted.
    const std::string reply = quote("Rex", "lunch?") + "yes";
    CHECK_EQ(quote("Ann", reply), std::string("<quote=Ann>yes</quote> "));
    // Just a quote, with nothing after it: what it quotes.
    CHECK_EQ(quote("Ann", quote("Rex", "lunch?")), std::string("<quote=Ann>lunch?</quote> "));
}

TEST(markup_quote_cannot_break_out) {
    const std::string message = quote("Re<x>", "a </quote> b") + "reply";
    const auto q = split_quote(message);
    REQUIRE(q.has_value());
    CHECK_EQ(std::string(q->name), std::string("Rex"));
    CHECK_EQ(std::string(q->reply), std::string("reply"));
}

TEST(markup_quote_shortened) {
    const std::string message = quote("Rex", std::string(500, 'a'));
    const auto q = split_quote(message);
    REQUIRE(q.has_value());
    CHECK(q->text.size() <= 200);
    CHECK(q->text.ends_with("…"));
}
