// Tests of how a Pokémon battle travels (pokemon_link.hpp), and of process::Child, which runs the simulator's bridge.

#include "check.hpp"

#include "pokemon_link.hpp"
#include "process.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace zchat::pokemon::link;

TEST(link_fields) {
    CHECK_EQ(encode_field("Sneaky Velociraptor"), "Sneaky%20Velociraptor");
    CHECK_EQ(encode_field("100% Ash"), "100%25%20Ash");
    CHECK_EQ(encode_field(""), "%");
    for (const std::string name : {"Sneaky Velociraptor", "100% Ash", "", "%20", "a%", "日本 語"}) {
        CHECK_EQ(decode_field(encode_field(name)), name);
    }
}

TEST(link_unescape) {
    CHECK_EQ(unescape("|move|a\\n|turn|2"), "|move|a\n|turn|2");
    CHECK_EQ(unescape("back\\\\slash\\\\n"), "back\\slash\\n");
    CHECK_EQ(unescape("end\\"), "end\\");
}

TEST(link_split) {
    const std::string short_message = "|turn|1";
    CHECK_EQ(split(short_message, 900).size(), std::size_t(1));
    // Never cut in a UTF-8 character: "é" is 2 bytes.
    std::string accents;
    for (int i = 0; i < 1000; ++i) {
        accents += "é";
    }
    const auto parts = split(accents, 901);
    std::string joined;
    for (const auto part : parts) {
        CHECK(part.size() <= 901);
        CHECK(part.size() % 2 == 0);
        joined += part;
    }
    CHECK_EQ(joined, accents);
    CHECK_EQ(parts.size(), std::size_t(3));
}

TEST(link_inbound_in_order_out_of_order_twice) {
    Inbound in;
    CHECK(in.add(1, 0, 1, "one") == std::vector<std::string> {"one"});
    // 3 before 2, and the parts of 2 backwards and twice.
    CHECK(in.add(3, 0, 1, "three").empty());
    CHECK(in.add(2, 1, 2, "B").empty());
    CHECK(in.add(2, 1, 2, "B").empty());
    CHECK((in.add(2, 0, 2, "A") == std::vector<std::string> {"AB", "three"}));
    CHECK_EQ(in.received(), std::uint64_t(3));
    // Old ones again are nothing new.
    CHECK(in.add(1, 0, 1, "one").empty());
    CHECK(in.add(3, 0, 1, "three").empty());
    CHECK_EQ(in.partial(), std::size_t(0));
}

TEST(link_inbound_rejects_nonsense) {
    Inbound in;
    CHECK(in.add(0, 0, 1, "zero").empty());
    CHECK(in.add(1, 2, 2, "part past the end").empty());
    CHECK(in.add(1, 0, 0, "no parts").empty());
    CHECK(in.add(1, 0, 100000, "too many parts").empty());
    CHECK(in.add(1'000'000, 0, 1, "too far ahead").empty());
    CHECK_EQ(in.partial(), std::size_t(0));
    // A part that disagrees on the count is not mixed in.
    CHECK(in.add(1, 0, 2, "A").empty());
    CHECK(in.add(1, 1, 3, "X").empty());
    CHECK(in.add(1, 1, 2, "B") == std::vector<std::string> {"AB"});
}

TEST(link_inbound_drops_stale) {
    Inbound in(std::chrono::seconds(30));
    CHECK(in.add(2, 0, 1, "two").empty());
    CHECK(in.add(1, 0, 2, "half").empty());
    in.drop_stale(Inbound::clock::now() + std::chrono::seconds(10));
    CHECK_EQ(in.partial(), std::size_t(2));
    in.drop_stale(Inbound::clock::now() + std::chrono::seconds(31));
    CHECK_EQ(in.partial(), std::size_t(0));
    // Sent again, they make it.
    CHECK(in.add(1, 0, 1, "one") == std::vector<std::string> {"one"});
}

TEST(process_child_lines_both_ways) {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> lines;
    bool ended = false;
    int code = -2;
    std::string error;
    // A shell, told to echo things.
#ifdef _WIN32
    const std::vector<std::string> args = {"cmd", "/d", "/q"};
#else
    const std::vector<std::string> args = {"sh"};
#endif
    auto child = zchat::process::Child::start(
        args,
        [&](std::string line) {
            std::scoped_lock lock(mutex);
            lines.push_back(std::move(line));
            cv.notify_all();
        },
        [&](int exit_code, std::string) {
            std::scoped_lock lock(mutex);
            ended = true;
            code = exit_code;
            cv.notify_all();
        },
        error);
    REQUIRE(child != nullptr);
    CHECK(child->write("echo hello"));
    CHECK(child->write("echo pokemon showdown"));
    const auto has = [&](std::string_view text) {
        // cmd shows its prompt before what it prints.
        return std::ranges::any_of(lines, [&](const std::string& line) {
            return line.ends_with(text);
        });
    };
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(10), [&] {
            return has("hello") && has("pokemon showdown");
        });
        CHECK(has("hello"));
        CHECK(has("pokemon showdown"));
    }
    // Going away: it is told (its input closes) and waited for, and nothing is called after.
    child.reset();
    std::scoped_lock lock(mutex);
    CHECK(!ended || code != -2);
}

TEST(process_child_missing_program) {
    std::string error;
    auto child = zchat::process::Child::start(
        {"zchat-no-such-program-here"},
        [](std::string) {
        },
        [](int, std::string) {
        },
        error);
    CHECK(child == nullptr);
    CHECK(!error.empty());
}
