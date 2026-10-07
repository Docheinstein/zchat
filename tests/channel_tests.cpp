// Tests of the channels' states (channel.hpp), private chats above all.

#include "check.hpp"

#include "channel.hpp"

#include <optional>
#include <string>

using namespace zchat::channel;

namespace {

Channel direct_chat(std::uint64_t a, std::uint64_t b, std::uint64_t author) {
    Channel c;
    c.name = direct_name(a, b);
    c.is_public = false;
    c.direct = true;
    c.owner = author;
    c.author = author;
    c.members = {a, b};
    return c;
}

} // namespace

TEST(channel_direct_name_is_the_same_for_both) {
    CHECK_EQ(direct_name(0x12, 0x34), direct_name(0x34, 0x12));
    CHECK(direct_name(0x12, 0x34) != direct_name(0x12, 0x35));
    const std::string name = direct_name(0xdeadbeef, 0xfeedface);
    CHECK(name.starts_with(direct_prefix));
    CHECK(name.size() <= max_name);
    CHECK_EQ(clean_name(name), name);
}

TEST(channel_direct_round_trip) {
    const Channel c = direct_chat(0x12, 0x34, 0x12);
    const auto back = decode(encode(c));
    REQUIRE(back.has_value());
    CHECK(back->direct);
    CHECK(!back->is_public);
    CHECK_EQ(back->name, c.name);
    CHECK(back->members == c.members);
}

TEST(channel_direct_decode_checks_it) {
    Channel c = direct_chat(0x12, 0x34, 0x12);
    // Its name is of its two.
    c.name = direct_name(0x12, 0x56);
    CHECK(!decode(encode(c)).has_value());
    // Always two.
    c = direct_chat(0x12, 0x34, 0x12);
    c.members.insert(0x56);
    CHECK(!decode(encode(c)).has_value());
    // Never deleted.
    c = direct_chat(0x12, 0x34, 0x12);
    c.deleted = true;
    CHECK(!decode(encode(c)).has_value());
    // And no other channel is called so.
    Channel other;
    other.name = direct_name(0x12, 0x34);
    other.owner = other.author = 0x12;
    other.members = {0x12};
    CHECK(!decode(encode(other)).has_value());
}

TEST(channel_direct_accepts) {
    const Channel c = direct_chat(0x12, 0x34, 0x12);
    CHECK(accepts(std::nullopt, c));
    // Made by somebody else: no.
    CHECK(!accepts(std::nullopt, direct_chat(0x12, 0x34, 0x56)));
    // Both opening it at once: the same one, whoever wins.
    CHECK(accepts(c, direct_chat(0x12, 0x34, 0x34)));
    CHECK(!accepts(direct_chat(0x12, 0x34, 0x34), c));
    // Nobody can turn it into a channel, nor a channel into one.
    Channel taken = c;
    taken.direct = false;
    taken.version = 2;
    CHECK(!accepts(c, taken));
    Channel plain;
    plain.name = c.name;
    plain.owner = plain.author = 0x12;
    plain.members = {0x12};
    CHECK(!accepts(plain, direct_chat(0x12, 0x34, 0x34)));
}

TEST(channel_plain_still_works) {
    Channel c;
    c.name = "team";
    c.owner = c.author = 0x12;
    c.members = {0x12};
    const auto back = decode(encode(c));
    REQUIRE(back.has_value());
    CHECK(!back->direct);
    CHECK(back->is_public);
    Channel joined = c;
    joined.version = 2;
    joined.author = 0x34;
    joined.members.insert(0x34);
    CHECK(accepts(c, joined));
}
