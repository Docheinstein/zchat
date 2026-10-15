// Tests of the commands suggested while one is typed (commands.hpp).

#include "check.hpp"

#include "commands.hpp"

#include <string>
#include <vector>

using namespace zchat::commands;

namespace {

// The names, separated by spaces.
std::string names(const std::vector<Command>& commands) {
    std::string out;
    for (const auto& c : commands) {
        out += (out.empty() ? "" : " ") + std::string(c.name);
    }
    return out;
}

} // namespace

TEST(commands_sorted_and_unique) {
    const auto list = all();
    REQUIRE(!list.empty());
    for (std::size_t i = 1; i < list.size(); ++i) {
        CHECK(list[i - 1].name < list[i].name);
    }
    for (const auto& c : list) {
        CHECK(c.name.starts_with('/'));
        CHECK(!c.about.empty());
    }
}

TEST(commands_matching) {
    CHECK_EQ(matching("/").size(), all().size());
    CHECK_EQ(names(matching("/co")), std::string("/coins /color"));
    CHECK_EQ(names(matching("/WHO")), std::string("/who /whoami"));
    CHECK_EQ(names(matching("/whoami")), std::string("/whoami"));
    CHECK(matching("/xyz").empty());
    CHECK(matching("/whoamii").empty());
    CHECK(matching("").empty());
    CHECK(matching("who").empty());
    CHECK(matching("//").empty());
    CHECK(matching("/who ").empty());
}

TEST(commands_runs_alone) {
    CHECK(runs_alone({"/who", "", "x"}));
    CHECK(runs_alone({"/coins", "[NAME|top]", "x"}));
    CHECK(!runs_alone({"/nick", "NAME", "x"}));
    CHECK(runs_alone({"/game", "[help NAME]", "x"}));
    CHECK(!runs_alone({"/image", "[SIZE] FILE", "x"}));
    CHECK(!runs_alone({"/add", "[#CHANNEL] NAME", "x"}));
}
