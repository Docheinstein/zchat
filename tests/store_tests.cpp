// Tests of what zchat keeps and syncs, and of the updates due later (store.hpp).

#include "check.hpp"

#include "store.hpp"

#include <filesystem>
#include <string>

using namespace zchat::store;

namespace {

constexpr std::time_t now = 2'000'000'000;

Record message(std::string topic, std::uint64_t seq, std::string text, std::time_t time = now - 60) {
    return {std::move(topic), key_of(0xabc, seq, text), time, 0xabc, 0x42, "Rex", 'm', text};
}

// A file of the test's own, gone before and after.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(std::string_view name) :
        path(std::filesystem::temp_directory_path() / std::format("zchat-test-{}", name)) {
        std::filesystem::remove(path);
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // namespace

TEST(store_keys) {
    CHECK(key_of(1, 2, "hi") == key_of(1, 2, "hi"));
    CHECK(key_of(1, 2, "hi") != key_of(1, 2, "ho"));
    CHECK(key_of(1, 2, "hi") != key_of(1, 3, "hi"));
    CHECK(valid_key(key_of(0xffffffffffffffffULL, 99999999999ULL, "x")));
    CHECK(!valid_key("Bad Key"));
    CHECK(!valid_key(""));
    CHECK(valid_topic("general"));
    CHECK(valid_topic("dm-1f"));
    CHECK(valid_topic("~wordle"));
    CHECK(!valid_topic("~"));
    CHECK(!valid_topic("no spaces"));
}

TEST(store_encode_round_trip) {
    Record r = message("general", 7, "a \"quoted\"\nline, with ünïcödé");
    r.kind = 'a';
    const auto back = decode(encode(r));
    REQUIRE(back.has_value());
    CHECK_EQ(back->topic, r.topic);
    CHECK_EQ(back->key, r.key);
    CHECK(back->time == r.time);
    CHECK(back->sender == r.sender);
    CHECK(back->user == r.user);
    CHECK_EQ(back->name, r.name);
    CHECK(back->kind == 'a');
    CHECK_EQ(back->data, r.data);
    CHECK(!back->local);
    CHECK(!decode("not json"));
    CHECK(!decode(R"(["general","k",1,"zz","0","n","m","d",0])"));
    CHECK(!decode(R"(["general","k",1,"1","0","n","x","d",0])"));
}

TEST(store_digest_and_missing) {
    Store a;
    Store b;
    CHECK(a.digest("general", now) == 0);
    for (std::uint64_t i = 1; i <= 3; ++i) {
        CHECK(a.add(message("general", i, std::format("m{}", i)), now));
    }
    // Known already.
    CHECK(!a.add(message("general", 1, "m1"), now));
    CHECK(b.add(message("general", 2, "m2"), now));
    CHECK(a.digest("general", now) != b.digest("general", now));
    // What b lacks of a's: the two others, the oldest first.
    const auto keys = b.keys("general", now);
    const std::set<std::string, std::less<>> theirs(keys.begin(), keys.end());
    const auto missing = a.missing("general", theirs, 1'000'000, now);
    REQUIRE(missing.size() == 2);
    CHECK_EQ(missing[0].data, std::string("m1"));
    CHECK_EQ(missing[1].data, std::string("m3"));
    for (const auto& r : missing) {
        b.add(r, now);
    }
    CHECK(a.digest("general", now) == b.digest("general", now));
    // Other topics are apart.
    CHECK(b.digest("dev", now) == 0);
    // Too little room: the newest first.
    CHECK(a.missing("general", {}, encode(message("general", 3, "m3")).size() + 1, now).size() == 1);
}

TEST(store_local_and_old) {
    Store s;
    Record local = message("general", 1, "from before");
    local.local = true;
    CHECK(s.add(local, now));
    // Kept, shown, but never told about.
    CHECK(s.records("general").size() == 1);
    CHECK(s.digest("general", now) == 0);
    CHECK(s.keys("general", now).empty());
    CHECK(s.missing("general", {}, 1'000'000, now).empty());
    // Too old to keep.
    CHECK(!s.add(message("general", 2, "old", now - max_age - 10), now));
}

TEST(store_file) {
    TempFile file("store");
    {
        Store s(file.path, now);
        s.add(message("general", 1, "one"), now);
        s.add(message("dev", 2, "two"), now);
    }
    Store again(file.path, now);
    CHECK(again.records("general").size() == 1);
    CHECK(again.records("dev").size() == 1);
    CHECK(again.has(key_of(0xabc, 2, "two")));
    // Forgotten once too old, when loaded later.
    Store later(file.path, now + max_age + 3 * 24 * 60 * 60);
    CHECK(later.topics().empty());
}

TEST(store_queue) {
    TempFile file("queue");
    {
        Queue q(file.path);
        q.add("wordle-end 2026-10-14", now + 10);
        q.add("wordle-end 2026-10-13", now - 10);
        q.add("other", now - 5);
        // Once by key.
        q.add("wordle-end 2026-10-14", now + 1000);
        CHECK(q.take_due("wordle-end ", now).size() == 1);
    }
    // Kept until done, also through a restart.
    Queue q(file.path);
    CHECK(q.waiting("wordle-end 2026-10-14"));
    CHECK(!q.waiting("wordle-end 2026-10-13"));
    const auto due = q.take_due("", now + 20);
    REQUIRE(due.size() == 2);
    CHECK_EQ(due[0], std::string("other"));
    CHECK_EQ(due[1], std::string("wordle-end 2026-10-14"));
    CHECK(q.take_due("", now + 20).empty());
}
