#include "image.hpp"

#include "color.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

// stb_image, built right here: only the decoders zchat needs, reading from memory (the file is read by zchat, so
// Unicode paths work everywhere).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_PSD
#define STBI_ONLY_PNM
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <stb_image.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace zchat::image {

namespace {

    // Bigger files or images are surely not meant to be turned into a few lines of text.
    constexpr std::uintmax_t max_file_bytes = 64 * 1024 * 1024;
    constexpr long long max_pixels = 64LL * 1024 * 1024;

    // Every character of a drawing gets its hue from a color code before it (see color_code()). With colors,
    // render() shows each one as a solid block, as bright as its place in the ramp (so pictures look the same in
    // every terminal and font), dimmed further by a brightness code (see dim_levels); quadrant blocks, for cells
    // with bright and dark parts, keep their shape. Without colors, the ASCII characters are shown as they are.
    constexpr std::string_view ramp = " .:-=+*#%@";

    // How bright the characters after a code are, as a fraction of what their place in the ramp makes them; every row
    // starts at full.
    struct DimLevel {
        char code;
        double level;
    };
    constexpr std::array dim_levels = std::to_array<DimLevel>({
        {'(', 0.08},
        {')', 0.12},
        {'[', 0.17},
        {']', 0.23},
        {'{', 0.3},
        {'}', 0.38},
        {'<', 0.47},
        {'>', 0.56},
        {'^', 0.66},
        {'~', 0.77},
        {';', 0.88},
        {'?', 1.0},
    });
    constexpr std::size_t full_level = dim_levels.size() - 1;

    std::optional<double> dim_of_code(char code) {
        if (code == '`') {
            return 0.0;
        }
        for (const auto& dim : dim_levels) {
            if (dim.code == code) {
                return dim.level;
            }
        }
        return std::nullopt;
    }

    // Indexed by the lit quarters: 1 top left, 2 top right, 4 bottom left, 8 bottom right.
    constexpr std::array<std::string_view, 16> quadrants = {" ", "▘", "▝", "▀", "▖", "▌", "▞", "▛",
                                                            "▗", "▚", "▐", "▜", "▄", "▙", "▟", "█"};

    // Two colors per character: a background, set by '&' followed by a color code and a brightness code ('|' goes
    // back to none, the terminal's), and a foreground for the ink of the character, which can be black ('`' as its
    // brightness). With them, the blocks below draw an edge anywhere in a character to an eighth of its size, and
    // keep thin dark outlines that would otherwise be averaged away. They are all drawn by the terminal itself in
    // Windows Terminal (and by most fonts) to fill exactly their part of the character.
    constexpr char background_code = '&';
    constexpr char no_background_code = '|';
    constexpr char black_code = '`';

    // The part of a character a block inks, on an 8 x 8 grid: bit row * 8 + column.
    struct Block {
        std::string_view glyph;
        std::uint64_t mask;
    };
    constexpr std::uint64_t rows_mask(int from, int to) {
        std::uint64_t mask = 0;
        for (int r = from; r < to; ++r) {
            mask |= std::uint64_t {0xFF} << (r * 8);
        }
        return mask;
    }
    constexpr std::uint64_t cols_mask(int from, int to) {
        std::uint64_t mask = 0;
        for (int r = 0; r < 8; ++r) {
            for (int c = from; c < to; ++c) {
                mask |= std::uint64_t {1} << (r * 8 + c);
            }
        }
        return mask;
    }
    constexpr std::uint64_t top_left = rows_mask(0, 4) & cols_mask(0, 4);
    constexpr std::uint64_t top_right = rows_mask(0, 4) & cols_mask(4, 8);
    constexpr std::uint64_t bottom_left = rows_mask(4, 8) & cols_mask(0, 4);
    constexpr std::uint64_t bottom_right = rows_mask(4, 8) & cols_mask(4, 8);
    constexpr std::array blocks = std::to_array<Block>({
        {"▀", rows_mask(0, 4)},
        {"▔", rows_mask(0, 1)},
        {"▁", rows_mask(7, 8)},
        {"▂", rows_mask(6, 8)},
        {"▃", rows_mask(5, 8)},
        {"▄", rows_mask(4, 8)},
        {"▅", rows_mask(3, 8)},
        {"▆", rows_mask(2, 8)},
        {"▇", rows_mask(1, 8)},
        {"▏", cols_mask(0, 1)},
        {"▎", cols_mask(0, 2)},
        {"▍", cols_mask(0, 3)},
        {"▌", cols_mask(0, 4)},
        {"▋", cols_mask(0, 5)},
        {"▊", cols_mask(0, 6)},
        {"▉", cols_mask(0, 7)},
        {"▐", cols_mask(4, 8)},
        {"▕", cols_mask(7, 8)},
        {"▘", top_left},
        {"▝", top_right},
        {"▖", bottom_left},
        {"▗", bottom_right},
        {"▚", top_left | bottom_right},
        {"▞", top_right | bottom_left},
        {"▛", top_left | top_right | bottom_left},
        {"▜", top_left | top_right | bottom_right},
        {"▙", top_left | bottom_left | bottom_right},
        {"▟", top_right | bottom_left | bottom_right},
    });

    // How much of its character a block inks, or nullopt for anything else.
    std::optional<double> block_coverage(std::string_view glyph) {
        if (glyph == "█") {
            return 1.0;
        }
        for (const auto& block : blocks) {
            if (block.glyph == glyph) {
                return static_cast<double>(std::popcount(block.mask)) / 64;
            }
        }
        return std::nullopt;
    }

    // How much closer to its pixels a character with two colors must be than one of a single color, as the squared
    // difference of their colors (0 to 3 for each of the 64 points): a shape only for real detail, which also saves
    // bytes.
    constexpr double detail_margin = 64 * 0.004;

    // Contrast is stretched only for washed out pictures, whose brightness spans less than this, and brightened at
    // most so much: a dark picture stays darker than a bright one, and a picture with good contrast as it is.
    constexpr double washed_out_range = 0.6;
    constexpr double max_gain = 2.0;
    // Darker than this is blank.
    constexpr double blank_below = 0.05;
    // A level already set is kept when it is this close to the right one, which saves a code.
    constexpr double keep_level_within = 0.05;
    // How much better quadrants must match than an even block to be used: only for real edges.
    constexpr double quadrant_margin = 0.04;

    constexpr std::array<std::string_view, 10> extensions = {".png", ".jpg", ".jpeg", ".gif", ".bmp",
                                                             ".tga", ".psd", ".ppm",  ".pgm", ".pnm"};

    std::string_view trim(std::string_view s) {
        while (!s.empty() && s.front() == ' ') {
            s.remove_prefix(1);
        }
        while (!s.empty() && s.back() == ' ') {
            s.remove_suffix(1);
        }
        return s;
    }

    int hex_value(char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    // Decodes the %xx escapes of a URL.
    std::string percent_decode(std::string_view s) {
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
                out += static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
                i += 2;
            } else {
                out += s[i];
            }
        }
        return out;
    }

    // Colors are written as one letter or digit, for 61 hues: the mixes of 5 levels of red, green and blue where
    // the strongest one is at full level. How dark a character looks is up to its glyph.
    constexpr std::string_view color_codes = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz012345678";
    constexpr int color_levels = 5;

    // The code of each mix, at index (r * 5 + g) * 5 + b, for levels from 0 to 4; 0 for the mixes that are not used.
    constexpr auto code_table = [] {
        std::array<char, color_levels * color_levels * color_levels> table {};
        std::size_t next = 0;
        for (std::size_t i = 0; i < table.size(); ++i) {
            const int r = static_cast<int>(i) / 25;
            const int g = static_cast<int>(i) / 5 % 5;
            const int b = static_cast<int>(i) % 5;
            if (std::max({r, g, b}) == color_levels - 1) {
                table[i] = color_codes[next++];
            }
        }
        return table;
    }();

    // The code of the hue closest to a color, of any brightness.
    char color_code(const std::array<double, 3>& rgb) {
        const double strongest = std::max({rgb[0], rgb[1], rgb[2]});
        if (strongest <= 0) {
            return code_table.back();
        }
        std::size_t index = 0;
        for (const double v : rgb) {
            index = index * color_levels + static_cast<std::size_t>(std::lround(v / strongest * (color_levels - 1)));
        }
        return code_table[index];
    }

    std::optional<Color> color_of_code(char code) {
        const auto it = std::ranges::find(code_table, code);
        if (code == 0 || it == code_table.end()) {
            return std::nullopt;
        }
        const auto index = static_cast<int>(it - code_table.begin());
        auto level = [](int l) {
            return static_cast<std::uint8_t>(l * 255 / (color_levels - 1));
        };
        return Color {level(index / 25), level(index / 5 % 5), level(index % 5)};
    }

    // The brightness of a pixel, from 0 to 1, as if it were on a black background. It is that of its strongest
    // channel, the one its color code keeps at full level: a pure red is as bright as a white, just redder.
    double brightness(const unsigned char* rgba) {
        return std::max({rgba[0], rgba[1], rgba[2]}) * static_cast<double>(rgba[3]) / (255.0 * 255.0);
    }

} // namespace

std::filesystem::path parse_path(std::string_view line) {
    const std::string_view s = trim(line);
    std::string raw;
    if (s.starts_with("file://")) {
        std::string_view url = s.substr(std::string_view("file://").size());
        if (url.starts_with("localhost/")) {
            url.remove_prefix(std::string_view("localhost").size());
        }
        raw = percent_decode(url);
#ifdef _WIN32
        // file:///C:/Users/me/cat.png
        if (raw.size() >= 3 && raw[0] == '/' && raw[2] == ':') {
            raw.erase(0, 1);
        }
#endif
    } else {
#ifdef _WIN32
        // Backslashes separate folders on Windows, so only quotes around the whole path are removed.
        raw = s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()
                  ? s.substr(1, s.size() - 2)
                  : s;
#else
        // Quoted the way a shell reads it, as terminals quote a dropped file: '/home/me/my cat.png',
        // "/home/me/my cat.png", /home/me/my\ cat.png, and a quote in the name like '/home/me/it'\''s.png'.
        char quote = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (quote == '\'') {
                if (c == '\'') {
                    quote = 0;
                } else {
                    raw += c;
                }
            } else if (c == '\\' && i + 1 < s.size() &&
                       (quote == 0 || std::string_view("\"\\$`").find(s[i + 1]) != std::string_view::npos)) {
                raw += s[++i];
            } else if (c == quote && quote == '"') {
                quote = 0;
            } else if (quote == 0 && (c == '\'' || c == '"')) {
                quote = c;
            } else {
                raw += c;
            }
        }
#endif
    }
    try {
        return std::filesystem::path(std::u8string(raw.begin(), raw.end()));
    } catch (const std::exception&) {
        // Not valid UTF-8 (from a bad %xx escape), so not a file name.
        return {};
    }
}

bool is_dropped_image(std::string_view line) {
    const std::filesystem::path path = parse_path(line);
    if (path.empty()) {
        return false;
    }
    std::string ext;
    for (const char8_t c : path.extension().u8string()) {
        ext += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (std::ranges::find(extensions, ext) == extensions.end()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

std::expected<std::string, std::string> to_ascii(const std::filesystem::path& path, std::size_t max_cols,
                                                 std::size_t max_rows) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected("There is no such file.");
    }
    if (size > max_file_bytes) {
        return std::unexpected("That file is too big.");
    }
    std::ifstream file(path, std::ios::binary);
    const std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file && !file.eof()) {
        return std::unexpected("Could not read the file.");
    }

    int w = 0;
    int h = 0;
    int channels = 0;
    const int len = static_cast<int>(data.size());
    if (!stbi_info_from_memory(data.data(), len, &w, &h, &channels) || w <= 0 || h <= 0) {
        return std::unexpected("That is not an image zchat can read (PNG, JPEG, GIF, BMP, TGA, PSD or PNM).");
    }
    if (static_cast<long long>(w) * h > max_pixels) {
        return std::unexpected("That image is too big.");
    }
    const std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(data.data(), len, &w, &h, &channels, 4), &stbi_image_free);
    if (!pixels) {
        return std::unexpected(std::string("Could not decode the image: ") + stbi_failure_reason());
    }

    // A character is about twice as tall as it is wide, so each one covers twice as many pixel rows as columns.
    const auto width = static_cast<std::size_t>(w);
    const auto height = static_cast<std::size_t>(h);
    std::size_t cols = std::clamp<std::size_t>(width, 1, max_cols);
    std::size_t rows = std::max<std::size_t>(1, std::llround(static_cast<double>(height) * cols / width / 2.0));
    if (rows > max_rows) {
        rows = max_rows;
        cols = std::clamp<std::size_t>(std::llround(static_cast<double>(width) * rows * 2.0 / height), 1, max_cols);
    }

    // The average brightness and color of the pixels under each character, and of each quarter of it
    // (top left, top right, bottom left, bottom right).
    std::vector<double> cells(rows * cols);
    std::vector<std::array<double, 4>> quarters(rows * cols);
    std::vector<char> codes(rows * cols);
    for (std::size_t r = 0; r < rows; ++r) {
        const std::size_t y0 = r * height / rows;
        const std::size_t y1 = std::max(y0 + 1, (r + 1) * height / rows);
        // With a single row (or column) of pixels, both halves are that row.
        const std::size_t ym = y1 - y0 >= 2 ? (y0 + y1) / 2 : y1;
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t x0 = c * width / cols;
            const std::size_t x1 = std::max(x0 + 1, (c + 1) * width / cols);
            const std::size_t xm = x1 - x0 >= 2 ? (x0 + x1) / 2 : x1;
            std::array<double, 4> sums {};
            std::array<double, 4> counts {};
            std::array<double, 3> rgb {};
            for (std::size_t y = y0; y < y1; ++y) {
                for (std::size_t x = x0; x < x1; ++x) {
                    const unsigned char* pixel = pixels.get() + (y * width + x) * 4;
                    const std::size_t q = (x < xm ? 0 : 1) + (y < ym ? 0 : 2);
                    sums[q] += brightness(pixel);
                    counts[q] += 1;
                    for (std::size_t k = 0; k < 3; ++k) {
                        rgb[k] += static_cast<double>(pixel[k]) * pixel[3];
                    }
                }
            }
            const std::size_t i = r * cols + c;
            cells[i] = (sums[0] + sums[1] + sums[2] + sums[3]) / (counts[0] + counts[1] + counts[2] + counts[3]);
            for (std::size_t q = 0; q < 4; ++q) {
                // A quarter without pixels (a cell one pixel wide or tall) looks like the whole cell.
                quarters[i][q] = counts[q] > 0 ? sums[q] / counts[q] : cells[i];
            }
            codes[i] = color_code(rgb);
        }
    }

    // Stretch the contrast of washed out pictures, so they use all the shades, but brighten dim ones only so much:
    // a dark picture should still look dark.
    const auto [lo, hi] = std::ranges::minmax(cells);
    if (hi - lo > 0.05 && hi - lo < washed_out_range) {
        const double gain = std::min(1 / (hi - lo), max_gain);
        const auto stretch = [&](double& v) {
            v = std::clamp((v - lo) * gain, 0.0, 1.0);
        };
        std::ranges::for_each(cells, stretch);
        for (auto& q : quarters) {
            std::ranges::for_each(q, stretch);
        }
    }

    // The ways of drawing a row: Rich with brightness codes and quadrants, Plain with the ramp only (like pictures
    // were first drawn), for when a row would not fit in a packet otherwise (see max_art_row_bytes).
    enum class Style { Rich, Plain };
    struct State {
        char color = 0;
        std::size_t dim = full_level;
    };
    struct Choice {
        std::string_view text = " ";
        std::size_t dim = full_level;
    };
    const auto ramp_char = [](std::size_t step) {
        return ramp.substr(step, 1);
    };
    const auto choose = [&](std::size_t i, const State& state, Style style) {
        const auto& q = quarters[i];
        const auto spread = [&](double shown) {
            return (std::abs(shown - q[0]) + std::abs(shown - q[1]) + std::abs(shown - q[2]) + std::abs(shown - q[3])) / 4;
        };
        const double last = static_cast<double>(ramp.size() - 1);
        if (cells[i] < blank_below) {
            return Choice {};
        }
        if (style == Style::Plain) {
            return Choice {ramp_char(static_cast<std::size_t>(std::lround(cells[i] * last)))};
        }

        // An even area: the ramp character bright enough for it (so the picture reads as ASCII art without colors),
        // dimmed to the right brightness.
        const auto step = std::clamp<std::size_t>(static_cast<std::size_t>(std::ceil(cells[i] * last)), 1, ramp.size() - 1);
        const double wanted = std::min(1.0, cells[i] * last / static_cast<double>(step));
        // The nearest level, or the one already set when it is close enough, which saves a code.
        std::size_t dim = state.dim;
        if (std::abs(dim_levels[dim].level - wanted) > keep_level_within) {
            dim = static_cast<std::size_t>(std::ranges::min_element(dim_levels, {}, [&](const DimLevel& l) {
                                               return std::abs(l.level - wanted);
                                           }) -
                                           dim_levels.begin());
        }
        const Choice uniform {ramp_char(step), dim};
        const double uniform_error = spread(dim_levels[dim].level * static_cast<double>(step) / last);

        // Bright and dark parts: quadrants, the lit quarters in the color at some level, the others dark, when they
        // look clearly better. (15, all lit, is an even block.)
        Choice quadrant;
        double quadrant_error = std::numeric_limits<double>::infinity();
        for (std::size_t lit = 1; lit < 15; ++lit) {
            for (std::size_t d = 0; d < dim_levels.size(); ++d) {
                double error = 0;
                for (std::size_t k = 0; k < 4; ++k) {
                    error += std::abs((lit >> k & 1 ? dim_levels[d].level : 0.0) - q[k]);
                }
                if (error / 4 < quadrant_error) {
                    quadrant_error = error / 4;
                    quadrant = {quadrants[lit], d};
                }
            }
        }
        return quadrant_error + quadrant_margin < uniform_error ? quadrant : uniform;
    };

    // The best way: two colors per character, and the block that makes them look the most like its pixels. Each
    // character is looked at as 8 x 8 points, with the average color of the pixels under each.
    struct Paint {
        char hue = 0;
        // Index in dim_levels, or nullopt for black.
        std::optional<std::size_t> dim;
        bool operator==(const Paint&) const = default;
    };
    const auto paint_of = [&](const std::array<double, 3>& rgb) {
        const double strongest = std::max({rgb[0], rgb[1], rgb[2]});
        if (strongest < blank_below) {
            return Paint {};
        }
        const auto dim = static_cast<std::size_t>(std::ranges::min_element(dim_levels, {}, [&](const DimLevel& l) {
                                                      return std::abs(l.level - strongest);
                                                  }) -
                                                  dim_levels.begin());
        return Paint {color_code(rgb), dim};
    };
    const auto two_color_row = [&](std::size_t r) {
        const std::size_t y0 = r * height / rows;
        const std::size_t y1 = std::max(y0 + 1, (r + 1) * height / rows);
        std::string line;
        // Each row starts over without a color, at full brightness and without a background.
        char hue = 0;
        std::size_t dim = full_level;
        bool black = false;
        std::optional<Paint> background;
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t x0 = c * width / cols;
            const std::size_t x1 = std::max(x0 + 1, (c + 1) * width / cols);
            std::array<std::array<double, 3>, 64> points {};
            for (std::size_t py = 0; py < 8; ++py) {
                const std::size_t ya = y0 + py * (y1 - y0) / 8;
                const std::size_t yb = std::max(ya + 1, y0 + (py + 1) * (y1 - y0) / 8);
                for (std::size_t px = 0; px < 8; ++px) {
                    const std::size_t xa = x0 + px * (x1 - x0) / 8;
                    const std::size_t xb = std::max(xa + 1, x0 + (px + 1) * (x1 - x0) / 8);
                    auto& point = points[py * 8 + px];
                    for (std::size_t y = ya; y < yb; ++y) {
                        for (std::size_t x = xa; x < xb; ++x) {
                            const unsigned char* pixel = pixels.get() + (y * width + x) * 4;
                            for (std::size_t k = 0; k < 3; ++k) {
                                // On a black background, like the terminal's.
                                point[k] += pixel[k] * pixel[3] / (255.0 * 255.0);
                            }
                        }
                    }
                    for (double& v : point) {
                        v /= static_cast<double>((yb - ya) * (xb - xa));
                    }
                }
            }
            // For a set of points in one color and the others in another, the best colors are their averages, and
            // what is left is how much the points differ from them.
            double squares = 0;
            std::array<double, 3> total {};
            for (const auto& p : points) {
                for (std::size_t k = 0; k < 3; ++k) {
                    squares += p[k] * p[k];
                    total[k] += p[k];
                }
            }
            const auto norm = [](const std::array<double, 3>& s) {
                return s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
            };
            const auto average = [](std::array<double, 3> s, double n) {
                for (double& v : s) {
                    v /= n;
                }
                return s;
            };
            const double even_error = squares - norm(total) / 64;
            double best_error = even_error;
            const Block* best = nullptr;
            std::array<double, 3> ink {};
            std::array<double, 3> rest {};
            for (const auto& block : blocks) {
                std::array<double, 3> in {};
                for (std::size_t b = 0; b < 64; ++b) {
                    if (block.mask >> b & 1) {
                        for (std::size_t k = 0; k < 3; ++k) {
                            in[k] += points[b][k];
                        }
                    }
                }
                const auto n_in = static_cast<double>(std::popcount(block.mask));
                const std::array<double, 3> out {total[0] - in[0], total[1] - in[1], total[2] - in[2]};
                const double error = squares - norm(in) / n_in - norm(out) / (64 - n_in);
                if (error + detail_margin < best_error) {
                    best_error = error + detail_margin;
                    best = &block;
                    ink = average(in, n_in);
                    rest = average(out, 64 - n_in);
                }
            }

            Paint fg = paint_of(best ? ink : average(total, 64));
            std::optional<Paint> bg;
            std::string_view glyph = "@"; // an even character: '@', a full block when shown (see render())
            if (best) {
                glyph = best->glyph;
                if (const Paint p = paint_of(rest); p.dim) {
                    bg = p;
                } else if (!fg.dim) {
                    // Both black.
                    glyph = " ";
                }
            } else if (!fg.dim) {
                glyph = " ";
            }

            if (glyph == " ") {
                // Spaces show the background: none, like the terminal's.
                if (background) {
                    line += no_background_code;
                    background.reset();
                }
                line += ' ';
                continue;
            }
            if (!fg.dim) {
                if (!black) {
                    line += black_code;
                    black = true;
                }
            } else {
                if (fg.hue != hue) {
                    hue = fg.hue;
                    line += hue;
                }
                if (black || *fg.dim != dim) {
                    dim = *fg.dim;
                    black = false;
                    line += dim_levels[dim].code;
                }
            }
            // A full character hides its background, which then stays as it is.
            if (best && bg != background) {
                if (bg) {
                    line += background_code;
                    line += bg->hue;
                    line += dim_levels[*bg->dim].code;
                } else {
                    line += no_background_code;
                }
                background = bg;
            }
            line += glyph;
        }
        if (background) {
            // Trailing spaces must not show a background either.
            line += no_background_code;
        }
        line.erase(line.find_last_not_of(std::string_view(" |")) + 1);
        return line;
    };

    std::vector<std::string> lines(rows);
    for (std::size_t r = 0; r < rows; ++r) {
        lines[r] = two_color_row(r);
        if (lines[r].size() <= max_art_row_bytes) {
            continue;
        }
        for (const Style style : {Style::Rich, Style::Plain}) {
            std::string& line = lines[r];
            line.clear();
            // Each row starts over without a color, at full brightness, so it can be read by itself.
            State state;
            for (std::size_t c = 0; c < cols; ++c) {
                const std::size_t i = r * cols + c;
                const Choice ch = choose(i, state, style);
                // Spaces do not show a color or a brightness, so they keep them.
                if (ch.text != " ") {
                    if (codes[i] != state.color) {
                        state.color = codes[i];
                        line += state.color;
                    }
                    if (ch.dim != state.dim) {
                        state.dim = ch.dim;
                        line += dim_levels[ch.dim].code;
                    }
                }
                line += ch.text;
            }
            line.erase(line.find_last_not_of(' ') + 1);
            if (line.size() <= max_art_row_bytes) {
                break;
            }
        }
    }

    // Drop the blank border (e.g. a transparent background), so the drawing is as small as it can be.
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    while (!lines.empty() && lines.front().empty()) {
        lines.erase(lines.begin());
    }
    if (lines.empty()) {
        return std::unexpected("That image is all black: there is nothing to draw.");
    }
    std::size_t indent = std::string::npos;
    for (const auto& line : lines) {
        if (!line.empty()) {
            indent = std::min(indent, line.find_first_not_of(' '));
        }
    }
    std::string art;
    for (const auto& line : lines) {
        art += art.empty() ? "" : "\n";
        art += line.empty() ? line : line.substr(indent);
    }
    return art;
}

std::string render(std::string_view art, bool colors) {
    // Each character becomes a solid block of its color, as dark as its glyph is thin: the picture looks the same
    // in every terminal, instead of depending on how its font draws the symbols and how much space it leaves between
    // lines. Only the way it is shown changes, so pictures from older versions look the same. The brightness codes
    // dim it further, and the blocks keep their shape, in their color on their background color. Without colors,
    // each character becomes the ASCII symbol as bright as it looks, which gives the black and white ASCII art.
    std::string out;
    std::optional<Color> hue;
    double level = 1.0;
    std::optional<Color> background;
    std::optional<Color> shown;
    std::optional<Color> shown_background;
    bool reading_background = false;
    const auto scaled = [](Color c, double by) {
        const auto shade = [&](std::uint8_t v) {
            return static_cast<std::uint8_t>(std::lround(v * by));
        };
        return Color {shade(c.r), shade(c.g), shade(c.b)};
    };
    const auto value = [](const std::optional<Color>& c) {
        return c ? std::max({c->r, c->g, c->b}) / 255.0 : 0.0;
    };
    // Shows a character of the given brightness (with colors) or looks (without).
    const auto show = [&](double brightness, bool with_background) {
        const std::optional<Color> fg = hue ? std::optional(scaled(*hue, brightness)) : std::nullopt;
        const std::optional<Color> bg = with_background ? background : std::nullopt;
        if (bg != shown_background) {
            out += bg ? std::format("\x1b[48;2;{};{};{}m", bg->r, bg->g, bg->b) : std::string("\x1b[49m");
            shown_background = bg;
        }
        if (fg && fg != shown) {
            out += std::format("\x1b[{}m", ansi_foreground(*fg));
            shown = fg;
        }
    };
    const auto ramp_symbol = [](double v) {
        return ramp[static_cast<std::size_t>(std::lround(std::clamp(v, 0.0, 1.0) * static_cast<double>(ramp.size() - 1)))];
    };
    const double last = static_cast<double>(ramp.size() - 1);
    for (std::size_t i = 0; i < art.size(); ++i) {
        const char c = art[i];
        if (reading_background) {
            // '&': a color code, then a brightness code.
            if (const auto color = color_of_code(c); color && i + 1 < art.size()) {
                background = scaled(*color, dim_of_code(art[i + 1]).value_or(1.0));
                ++i;
            }
            reading_background = false;
            continue;
        }
        if (const auto color = color_of_code(c)) {
            hue = color;
            continue;
        }
        if (const auto dim = dim_of_code(c)) {
            level = *dim;
            if (level == 0 && !hue) {
                hue = Color {};
            }
            continue;
        }
        if (c == background_code) {
            reading_background = true;
            continue;
        }
        if (c == no_background_code) {
            background.reset();
            continue;
        }
        if (c == '\n') {
            // Each row starts over without a color, at full brightness and without a background, like in to_ascii().
            if (colors && (shown || shown_background)) {
                out += "\x1b[0m";
            }
            hue.reset();
            background.reset();
            shown.reset();
            shown_background.reset();
            level = 1.0;
            out += c;
            continue;
        }
        const auto step = ramp.find(c);
        if (step != std::string_view::npos && c != ' ') {
            const double brightness = level * static_cast<double>(step) / last;
            if (!colors) {
                out += hue ? ramp_symbol(brightness * value(hue)) : c;
            } else if (hue) {
                show(brightness, false);
                out += "\u2588"; // █
            } else {
                out += c;
            }
            continue;
        }
        // A UTF-8 character (a block): all its bytes.
        std::size_t len = 1;
        while (i + len < art.size() && (static_cast<unsigned char>(art[i + len]) & 0xC0) == 0x80) {
            ++len;
        }
        const std::string_view glyph = art.substr(i, len);
        i += len - 1;
        if (!colors) {
            // As bright as it looks: its ink in its color, the rest in the background.
            if (const auto coverage = block_coverage(glyph)) {
                out += ramp_symbol(*coverage * level * value(hue) + (1 - *coverage) * value(background));
            } else {
                out += glyph;
            }
            continue;
        }
        show(level, true);
        out += glyph;
    }
    if (colors && (shown || shown_background)) {
        out += "\x1b[0m";
    }
    return out;
}

} // namespace zchat::image
