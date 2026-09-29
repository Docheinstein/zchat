#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace zchat {

// A name color.
struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    bool operator==(const Color&) const = default;
};

// The named colors to pick from.
std::size_t palette_size();
std::string_view palette_name(std::size_t index);
Color palette_color(std::size_t index);

// Reads a color: a palette name (case-insensitive), a hex code ("#ff8800", "ff8800", "#f80"), or RGB values from
// 0 to 255 ("255,136,0", "255 136 0", "rgb(255, 136, 0)").
std::optional<Color> parse_color(std::string_view text);

// The palette name of a color, or its hex code; parse_color() reads it back.
std::string describe(Color color);

// The ANSI SGR parameters that set a color as the foreground ("38;2;R;G;B").
std::string ansi_foreground(Color color);

// Peer ids carry the color of their name, so every peer shows it without it being sent:
// - a palette color is id % palette_size(), which is all that zchat versions before custom colors understand;
// - any other color is stored in bits 24-47 of an id tagged in its top 16 bits, with the random low 24 bits
//   chosen so that id % palette_size() is still the closest palette color, for those older versions.
Color color_of_id(std::uint64_t id);

// A new random id, for the given color or for a random palette color.
std::uint64_t make_id(std::optional<Color> color);

} // namespace zchat
