// Tests of the Unicode emoji and their :shortcodes: (emojis.hpp), with the real list of third_party/gemoji.

#include "check.hpp"

#include "emojis.hpp"

#include <fstream>
#include <iterator>
#include <string>

using namespace zchat::emojis;

namespace {

const std::vector<Emoji>& table() {
    static const std::vector<Emoji> t = [] {
        std::ifstream in(ZCHAT_GEMOJI, std::ios::binary);
        return parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
    }();
    return t;
}

} // namespace

TEST(emojis_parse) {
    REQUIRE(table().size() > 1500);
    const Emoji* rofl = find("rofl", table());
    REQUIRE(rofl != nullptr);
    CHECK_EQ(rofl->emoji, std::string("🤣"));
    CHECK_EQ(rofl->category, std::string("Smileys & Emotion"));
    // By any of its names, or by itself.
    CHECK(find("+1", table()) == find("thumbsup", table()));
    CHECK(find("👍", table()) == find("+1", table()));
    CHECK(find("not an emoji", table()) == nullptr);
    CHECK(parse("not json").empty());
}

TEST(emojis_replace) {
    CHECK_EQ(replace("lol :rofl:", table()), std::string("lol 🤣"));
    CHECK_EQ(replace(":+1::heart:", table()), std::string("👍❤️"));
    // Colons around anything else stay.
    CHECK_EQ(replace("at 12:30:45", table()), std::string("at 12:30:45"));
    CHECK_EQ(replace(":nope: :joy:", table()), std::string(":nope: 😂"));
    CHECK_EQ(replace("x:zq:joy: c", table()), std::string("x:zq😂 c"));
    // (":b:" is an emoji, 🅱️.)
    CHECK_EQ(replace(":b:", table()), std::string("🅱️"));
    CHECK_EQ(replace("http://x.com:8080/", table()), std::string("http://x.com:8080/"));
    CHECK_EQ(replace(":", table()), std::string(":"));
    CHECK_EQ(replace("", table()), std::string(""));
}

TEST(emojis_search) {
    const auto found = search("rof", 5, table());
    REQUIRE(!found.empty());
    CHECK_EQ(found.front()->aliases.front(), std::string("rofl"));
    // By a tag ("haha" is one of 😃's).
    const auto haha = search("haha", 50, table());
    CHECK(std::ranges::find(haha, find("smiley", table())) != haha.end());
    CHECK(search("zzzzqqq", 5, table()).empty());
    CHECK(search("a", 3, table()).size() == 3);
}
