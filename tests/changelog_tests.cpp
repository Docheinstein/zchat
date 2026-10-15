// Tests of reading CHANGELOG.md (changelog.hpp).

#include "check.hpp"

#include "changelog.hpp"

#include <fstream>
#include <iterator>
#include <string>

using namespace zchat::changelog;

namespace {

const char* const sample = "# Changelog\n"
                           "\n"
                           "Some words before the days, and * not an item.\n"
                           "\n"
                           "## 2026-10-15\n"
                           "\n"
                           "* **Do Not Disturb**, for a while: keeps every\n"
                           "  notification quiet.\n"
                           "* Windows builds with `/MP`: faster.\n"
                           "\n"
                           "## 2026-10-14\n"
                           "\n"
                           "- **Popups**: in the corner.\n"
                           "not part of the item\n"
                           "\n"
                           "## 2026-10-01\n"
                           "\n"
                           "* **Coins**: for games.\n";

} // namespace

TEST(changelog_parse) {
    const auto days = parse(sample);
    REQUIRE(days.size() == 3);
    CHECK_EQ(days[0].date, std::string("2026-10-15"));
    REQUIRE(days[0].items.size() == 2);
    CHECK_EQ(days[0].items[0], std::string("**Do Not Disturb**, for a while: keeps every notification quiet."));
    CHECK_EQ(days[0].items[1], std::string("Windows builds with `/MP`: faster."));
    REQUIRE(days[1].items.size() == 1);
    CHECK_EQ(days[1].items[0], std::string("**Popups**: in the corner."));
    CHECK(parse("").empty());
    CHECK(parse("* an item with no day\n").empty());
}

TEST(changelog_plain_and_headline) {
    CHECK_EQ(plain("**Bold** and `code` stay"), std::string("Bold and code stay"));
    CHECK_EQ(headline("**Do Not Disturb**, for a while"), std::string("Do Not Disturb"));
    CHECK_EQ(headline("Windows builds with `/MP`: faster."), std::string("Windows builds with /MP"));
    CHECK_EQ(headline("Short."), std::string("Short"));
    const auto longer = headline(std::string(30, 'a') + " " + std::string(40, 'b') + " end");
    CHECK_EQ(longer, std::string(30, 'a') + "…");
}

TEST(changelog_news_since) {
    const auto days = parse(sample);
    // Never seen: the newest day only.
    CHECK_EQ(news_since(days, "", 10).size(), std::size_t {2});
    const auto news = news_since(days, "2026-10-01", 10);
    REQUIRE(news.size() == 3);
    CHECK_EQ(news[0], std::string("Do Not Disturb"));
    CHECK_EQ(news[2], std::string("Popups"));
    CHECK_EQ(news_since(days, "2026-10-01", 2).size(), std::size_t {2});
    CHECK(news_since(days, "2026-10-15", 10).empty());
}

TEST(changelog_of_zchat) {
    // CHANGELOG.md itself: days newest first, each with items.
    std::ifstream in(ZCHAT_CHANGELOG);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto days = parse(text);
    REQUIRE(!days.empty());
    for (std::size_t i = 0; i < days.size(); ++i) {
        CHECK(days[i].date.size() == 10);
        CHECK(!days[i].items.empty());
        if (i > 0) {
            CHECK(days[i].date < days[i - 1].date);
        }
    }
}
