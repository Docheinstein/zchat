#include "color.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <random>
#include <vector>

namespace zchat {

namespace {

    struct NamedColor {
        std::string_view name;
        Color color;
    };

    // Exact colors rather than the terminal's palette, where e.g. red and bright red often look the same.
    // The order is part of the protocol: it maps ids to colors.
    constexpr std::array palette = std::to_array<NamedColor>({
        {"red", {230, 50, 50}},
        {"green", {60, 180, 75}},
        {"yellow", {240, 220, 40}},
        {"blue", {50, 110, 240}},
        {"magenta", {210, 40, 200}},
        {"cyan", {0, 200, 210}},
        {"pink", {255, 130, 185}},
        {"lime", {170, 240, 30}},
        {"gold", {255, 165, 0}},
        {"sky", {135, 195, 255}},
        {"violet", {150, 100, 255}},
        {"aqua", {40, 230, 160}},
    });

    constexpr std::uint64_t custom_tag = 0xC01D;
    constexpr int custom_tag_shift = 48;

    bool is_custom(std::uint64_t id) {
        return id >> custom_tag_shift == custom_tag;
    }

    std::uint64_t random_u64() {
        thread_local std::mt19937_64 rng(std::random_device {}());
        return rng();
    }

    std::size_t closest_palette_index(Color c) {
        std::size_t best = 0;
        int best_distance = -1;
        for (std::size_t i = 0; i < palette.size(); ++i) {
            const Color p = palette[i].color;
            const int dr = p.r - c.r;
            const int dg = p.g - c.g;
            const int db = p.b - c.b;
            const int distance = dr * dr + dg * dg + db * db;
            if (best_distance < 0 || distance < best_distance) {
                best = i;
                best_distance = distance;
            }
        }
        return best;
    }

    std::optional<std::size_t> find_palette_index(Color c) {
        for (std::size_t i = 0; i < palette.size(); ++i) {
            if (palette[i].color == c) {
                return i;
            }
        }
        return std::nullopt;
    }

    std::string lowercase(std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        return out;
    }

    std::optional<Color> parse_hex(std::string_view s) {
        if (s.starts_with('#')) {
            s.remove_prefix(1);
        }
        if (s.size() != 3 && s.size() != 6) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, 16);
        if (ec != std::errc {} || ptr != s.data() + s.size()) {
            return std::nullopt;
        }
        if (s.size() == 3) {
            // #f80 is #ff8800.
            const auto digit = [&](int shift) {
                return static_cast<std::uint8_t>(((value >> shift) & 0xF) * 0x11);
            };
            return Color {digit(8), digit(4), digit(0)};
        }
        return Color {static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 8),
                      static_cast<std::uint8_t>(value)};
    }

    std::optional<Color> parse_rgb(std::string_view s) {
        if (s.starts_with("rgb(") && s.ends_with(')')) {
            s = s.substr(4, s.size() - 5);
        }
        std::vector<std::uint8_t> parts;
        while (!s.empty()) {
            const auto start = s.find_first_not_of(", ");
            if (start == std::string_view::npos) {
                break;
            }
            s.remove_prefix(start);
            unsigned value = 0;
            const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
            if (ec != std::errc {} || value > 255 || (ptr != s.data() + s.size() && *ptr != ',' && *ptr != ' ')) {
                return std::nullopt;
            }
            parts.push_back(static_cast<std::uint8_t>(value));
            s.remove_prefix(static_cast<std::size_t>(ptr - s.data()));
        }
        if (parts.size() != 3) {
            return std::nullopt;
        }
        return Color {parts[0], parts[1], parts[2]};
    }

} // namespace

std::size_t palette_size() {
    return palette.size();
}

std::string_view palette_name(std::size_t index) {
    return palette[index % palette.size()].name;
}

Color palette_color(std::size_t index) {
    return palette[index % palette.size()].color;
}

std::optional<Color> parse_color(std::string_view text) {
    const std::string s = lowercase(text);
    for (const auto& entry : palette) {
        if (entry.name == s) {
            return entry.color;
        }
    }
    // "255 136 0" would also be a valid 3 digit hex code otherwise; separators make it RGB values.
    if (s.find_first_of(", (") != std::string::npos) {
        return parse_rgb(s);
    }
    return parse_hex(s);
}

std::string describe(Color color) {
    if (const auto index = find_palette_index(color)) {
        return std::string(palette[*index].name);
    }
    return std::format("#{:02x}{:02x}{:02x}", color.r, color.g, color.b);
}

std::string ansi_foreground(Color color) {
    return std::format("38;2;{};{};{}", color.r, color.g, color.b);
}

Color color_of_id(std::uint64_t id) {
    if (is_custom(id)) {
        return Color {static_cast<std::uint8_t>(id >> 40), static_cast<std::uint8_t>(id >> 32),
                      static_cast<std::uint8_t>(id >> 24)};
    }
    return palette[id % palette.size()].color;
}

std::uint64_t make_id(std::optional<Color> color) {
    const auto index = color ? find_palette_index(*color) : std::optional<std::size_t> {random_u64() % palette.size()};
    if (index) {
        // A plain id with the right remainder; never 0, and never looking like a custom color.
        while (true) {
            std::uint64_t id = random_u64();
            id = id - id % palette.size() + *index;
            // (The adjustment can wrap around near the top of the range, hence the remainder check.)
            if (id != 0 && !is_custom(id) && id % palette.size() == *index) {
                return id;
            }
        }
    }
    // Tag, the color, 24 random bits: retry the random bits until older versions also get the closest color.
    // (The random bits are the lowest ones so that every remainder can be reached.)
    const std::uint64_t rgb = (std::uint64_t {color->r} << 16) | (std::uint64_t {color->g} << 8) | color->b;
    const std::size_t closest = closest_palette_index(*color);
    while (true) {
        const std::uint64_t id = (custom_tag << custom_tag_shift) | (rgb << 24) | (random_u64() & 0xFFFFFF);
        if (id % palette.size() == closest) {
            return id;
        }
    }
}

} // namespace zchat
