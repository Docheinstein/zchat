#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace zchat::changelog {

// CHANGELOG.md, built into the executable: what is new in zchat, a day at a time.
std::string_view text();

// A day of the changelog: its "## YYYY-MM-DD" heading, and its "* " items, as written (with **bold** and `code`),
// their lines joined.
struct Day {
    std::string date;
    std::vector<std::string> items;
};

// The days of a changelog, as written in it: newest first. Anything before the first day is left out.
std::vector<Day> parse(std::string_view markdown);

// An item without its ** and `, for the terminal.
std::string plain(std::string_view item);

// What an item is about, to list it in a line: its first words in bold, or else its start, shortened.
std::string headline(std::string_view item);

// The headlines of the days newer than seen (an ISO date; empty: only the newest day), newest first, up to max.
std::vector<std::string> news_since(const std::vector<Day>& days, std::string_view seen, std::size_t max);

} // namespace zchat::changelog
