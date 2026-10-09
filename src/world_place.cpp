#include "world_place.hpp"

#include "markup.hpp"

#include <charconv>
#include <format>

namespace zchat::world {

namespace {

    std::optional<int> parse_int(std::string_view s) {
        int value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size()) {
            return std::nullopt;
        }
        return value;
    }

    std::optional<bool> parse_flag(std::string_view s) {
        if (s == "0" || s == "1") {
            return s == "1";
        }
        return std::nullopt;
    }

    // Takes the next word of s, and removes it with the space after it.
    std::string_view next_word(std::string_view& s) {
        const auto at = s.find(' ');
        const std::string_view word = s.substr(0, at);
        s.remove_prefix(at == std::string_view::npos ? s.size() : at + 1);
        return word;
    }

} // namespace

std::string encode_place(const Place& place) {
    return std::format("{} {} {} {} {}", place.room, place.x, place.y, place.left ? 1 : 0, place.walking ? 1 : 0);
}

std::optional<Place> parse_place(std::string_view text) {
    const std::string_view room = next_word(text);
    const auto x = parse_int(next_word(text));
    const auto y = parse_int(next_word(text));
    const auto left = parse_flag(next_word(text));
    const auto walking = parse_flag(next_word(text));
    if (!text.empty() || room.empty() || room.size() > 32 || !x || !y || !left || !walking) {
        return std::nullopt;
    }
    for (const char c : room) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return std::nullopt;
        }
    }
    constexpr int most = max_room_tiles * 100;
    if (*x < 0 || *y < 0 || *x > most || *y > most) {
        return std::nullopt;
    }
    return Place {std::string(room), *x, *y, *left, *walking};
}

std::string bubble_text(std::string_view message, std::size_t max_chars) {
    if (const auto quote = markup::split_quote(message)) {
        message = quote->reply;
    }
    const std::string plain = markup::render(message, false);
    std::string out;
    std::size_t chars = 0;
    bool space = false;
    for (std::size_t i = 0; i < plain.size(); ++i) {
        const char c = plain[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = !out.empty();
            continue;
        }
        // A character starts with any byte but a UTF-8 continuation byte (10xxxxxx).
        const bool starts = (static_cast<unsigned char>(c) & 0xC0) != 0x80;
        if (starts && chars + (space ? 1 : 0) >= max_chars) {
            return out + "…";
        }
        if (space) {
            out += ' ';
            ++chars;
            space = false;
        }
        out += c;
        chars += starts ? 1 : 0;
    }
    return out;
}

} // namespace zchat::world
