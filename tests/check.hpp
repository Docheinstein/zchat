#pragma once

// A small test framework for zchat_tests. A test is a function defined anywhere with TEST(name) { ... }; inside it,
// CHECK(condition) and CHECK_EQ(a, b) report what failed (and go on, so that one run shows every failure), and
// REQUIRE(condition) also ends the test when it fails. The main() in check.cpp runs them all, or those whose name
// contains its argument, and fails when any check did.

#include <format>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace zchat::check {

struct Test {
    std::string name;
    std::function<void()> run;
};

inline std::vector<Test>& tests() {
    static std::vector<Test> all;
    return all;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Register {
    Register(std::string name, std::function<void()> run) {
        tests().push_back({std::move(name), std::move(run)});
    }
};

// What a test stops with, when a REQUIRE fails.
struct Stop {};

inline void fail(std::string_view file, int line, std::string_view what) {
    ++failures();
    std::cerr << std::format("{}({}): check failed: {}\n", file, line, what);
}

// A value as a failed CHECK_EQ shows it.
template <typename T>
std::string show(const T& value) {
    if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        return std::format("\"{}\"", std::string_view(value));
    } else if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else {
        std::ostringstream out;
        out << value;
        return out.str();
    }
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, std::string_view file, int line, std::string_view text) {
    if (!(a == b)) {
        fail(file, line, std::format("{}: {} != {}", text, show(a), show(b)));
    }
}

} // namespace zchat::check

#define ZCHAT_CHECK_CONCAT2(a, b) a##b
#define ZCHAT_CHECK_CONCAT(a, b) ZCHAT_CHECK_CONCAT2(a, b)

#define TEST(name)                                                                                                     \
    static void ZCHAT_CHECK_CONCAT(test_, name)();                                                                     \
    static const ::zchat::check::Register ZCHAT_CHECK_CONCAT(register_, name)(#name, ZCHAT_CHECK_CONCAT(test_, name)); \
    static void ZCHAT_CHECK_CONCAT(test_, name)()

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            ::zchat::check::fail(__FILE__, __LINE__, #condition);                                                      \
        }                                                                                                              \
    } while (false)

#define CHECK_EQ(a, b) ::zchat::check::check_eq((a), (b), __FILE__, __LINE__, #a " == " #b)

#define REQUIRE(condition)                                                                                             \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            ::zchat::check::fail(__FILE__, __LINE__, #condition);                                                      \
            throw ::zchat::check::Stop {};                                                                             \
        }                                                                                                              \
    } while (false)
