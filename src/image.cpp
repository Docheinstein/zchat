#include "image.hpp"

#include "color.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <utility>
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
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace zchat::image {

namespace {

    // Bigger files or images are surely not meant to be turned into a few lines of text.
    constexpr std::uintmax_t max_file_bytes = 64 * 1024 * 1024;
    constexpr long long max_pixels = 64LL * 1024 * 1024;
    // The biggest a picture that cannot be sent as it is gets made again at.
    constexpr int max_remade_size = 1280;

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

    // How much closer to its pixels a character with two colors must be than one of a single color, as the absolute
    // difference of their colors (0 to 3 for each of the 64 points): a shape only for real detail, which also saves
    // bytes.
    constexpr double detail_margin = 64 * 0.03;

    // How much texture a picture gets, from 0 (none: blocks only) to 100, see to_ascii(). Texture draws dark
    // characters as text: ASCII symbols shown as themselves, on black, instead of blocks. It starts with the darkest,
    // and as it grows more and more of the dark range becomes text, up to, at 100, all that the densest symbol can
    // show; what is brighter keeps its blocks, which draw corners and thin lines that symbols cannot. Each character
    // gets the symbol with the least ink that reaches its light at texture_level or less (sparse strokes at full
    // brightness look darker than they should).
    //
    // How much of its character each one inks, measured in Cascadia Mono, and for the strokes a bit less, as thin
    // strokes look darker than the ink they cover. Bold symbols have more ink. From the least ink to the most.
    struct Texture {
        std::string_view glyph;
        double ink;
        bool bold = false;
    };
    constexpr double stroke_seen = 0.6;
    constexpr std::array textures = std::to_array<Texture>({
        {".", 0.03 * stroke_seen},
        {":", 0.059 * stroke_seen},
        {"-", 0.073 * stroke_seen},
        {"+", 0.141 * stroke_seen},
        {"=", 0.146 * stroke_seen},
        {"*", 0.163 * stroke_seen},
        {"%", 0.29 * stroke_seen},
        {"#", 0.296 * stroke_seen},
        {"@", 0.369 * stroke_seen},
        {"#", 0.379 * stroke_seen, true},
        {"W", 0.404 * stroke_seen, true},
        {"@", 0.44 * stroke_seen, true},
    });
    constexpr double texture_level = 0.6;

    // The luminance below which a character is text: from nothing at 0, through about 0.2 at 15, to the brightest the
    // densest symbol can show at 100.
    double texture_below(int texture) {
        const double brightest = std::pow(textures.back().ink, 1 / 2.2);
        return texture <= 0 ? 0.0 : brightest * std::sqrt(texture / 100.0);
    }
    // How much brighter than what the densest symbol can show (in its strongest channel) a character can be and still
    // be text, a little dimmer than it is.
    constexpr double reach_tolerance = 1.2;
    // How many of its 64 points must be darker than near_black for a character to be text: half of them up to 15
    // (only the shadows of drawings, the smooth dim parts of photos stay blocks), then fewer, none from 50.
    constexpr double near_black = 0.06;
    std::ptrdiff_t min_black_points(int texture) {
        return std::lround(32 * std::clamp(1 - (texture - 15) / 35.0, 0.0, 1.0));
    }
    // A picture whose bright parts are at least this bright gets texture below texture_below(); darker ones get it
    // lower, less so as the texture grows.
    constexpr double bright_luminance = 0.7;
    // Shows the symbol after it as itself instead of as a block (see render()).
    constexpr char literal_code = '"';
    // Switches bold on or off for the texture symbols after it (see render()); every row starts without.
    constexpr char bold_code = '!';

    // Light adds up, values (like those of pixels) do not: a character that inks half of it in a color looks like
    // the whole of it in a color with half the light, which is not half the value (the gamma of screens).
    double to_light(double value) {
        return std::pow(value, 2.2);
    }
    double to_value(double light) {
        return std::pow(light, 1 / 2.2);
    }

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

    // A picture comes with its own palette: a first row of palette_code followed by 3 characters per color (one per
    // channel, 64 levels, see palette_digits), in the order of color_codes. The color codes of the drawing then refer
    // to it instead of to the fixed hues, which gives each picture the colors it needs (skin, the white of an eye).
    constexpr char palette_code = '$';
    constexpr std::string_view palette_digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    // The colors that represent a picture best: k-means over the points of its characters, leaving out the black
    // ones (black is always there), rounded to what the palette row can say.
    std::vector<std::array<double, 3>> make_palette(const std::vector<std::array<std::array<double, 3>, 64>>& cells) {
        using Rgb = std::array<double, 3>;
        std::vector<Rgb> samples;
        for (const auto& points : cells) {
            for (const Rgb& p : points) {
                if (std::max({p[0], p[1], p[2]}) >= blank_below) {
                    samples.push_back(p);
                }
            }
        }
        // Enough of them to find the colors, few enough to be quick.
        constexpr std::size_t max_samples = 24000;
        if (samples.size() > max_samples) {
            std::vector<Rgb> fewer;
            for (std::size_t i = 0; i < max_samples; ++i) {
                fewer.push_back(samples[i * samples.size() / max_samples]);
            }
            samples = std::move(fewer);
        }
        if (samples.empty()) {
            return {};
        }
        const auto distance = [](const Rgb& a, const Rgb& b) {
            return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
        };
        // k-means++: each new center far from the others, with a fixed seed so a picture always gives the same.
        std::mt19937 rng(12345);
        const std::size_t k = std::min(color_codes.size(), samples.size());
        std::vector<Rgb> centers {samples[rng() % samples.size()]};
        std::vector<double> nearest(samples.size(), std::numeric_limits<double>::infinity());
        while (centers.size() < k) {
            double sum = 0;
            for (std::size_t i = 0; i < samples.size(); ++i) {
                nearest[i] = std::min(nearest[i], distance(samples[i], centers.back()));
                sum += nearest[i];
            }
            if (sum <= 0) {
                break;
            }
            double pick = std::uniform_real_distribution<double>(0, sum)(rng);
            std::size_t chosen = 0;
            while (chosen + 1 < samples.size() && (pick -= nearest[chosen]) > 0) {
                ++chosen;
            }
            centers.push_back(samples[chosen]);
        }
        std::vector<std::size_t> owner(samples.size());
        for (int iteration = 0; iteration < 12; ++iteration) {
            std::vector<Rgb> sums(centers.size());
            std::vector<double> counts(centers.size());
            for (std::size_t i = 0; i < samples.size(); ++i) {
                std::size_t best = 0;
                for (std::size_t c = 1; c < centers.size(); ++c) {
                    if (distance(samples[i], centers[c]) < distance(samples[i], centers[best])) {
                        best = c;
                    }
                }
                owner[i] = best;
                for (std::size_t ch = 0; ch < 3; ++ch) {
                    sums[best][ch] += samples[i][ch];
                }
                counts[best] += 1;
            }
            for (std::size_t c = 0; c < centers.size(); ++c) {
                if (counts[c] > 0) {
                    centers[c] = {sums[c][0] / counts[c], sums[c][1] / counts[c], sums[c][2] / counts[c]};
                }
            }
        }
        const double top = static_cast<double>(palette_digits.size() - 1);
        for (Rgb& c : centers) {
            for (double& v : c) {
                v = std::round(std::clamp(v, 0.0, 1.0) * top) / top;
            }
        }
        return centers;
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

std::expected<std::string, std::string> read_image_file(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected("There is no such file.");
    }
    if (size > max_file_bytes) {
        return std::unexpected("That file is too big.");
    }
    std::ifstream file(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file && !file.eof()) {
        return std::unexpected("Could not read the file.");
    }
    return data;
}

std::expected<DecodedImage, std::string> decode_image(std::string_view encoded) {
    int w = 0;
    int h = 0;
    int channels = 0;
    const auto* bytes = reinterpret_cast<const stbi_uc*>(encoded.data());
    const int len = static_cast<int>(std::min<std::size_t>(encoded.size(), std::numeric_limits<int>::max()));
    if (!stbi_info_from_memory(bytes, len, &w, &h, &channels) || w <= 0 || h <= 0) {
        return std::unexpected("That is not an image zchat can read (PNG, JPEG, GIF, BMP, TGA, PSD or PNM).");
    }
    if (static_cast<long long>(w) * h > max_pixels) {
        return std::unexpected("That image is too big.");
    }
    DecodedImage out {{nullptr, &stbi_image_free}, w, h};
    out.pixels.reset(stbi_load_from_memory(bytes, len, &w, &h, &channels, 4));
    if (!out.pixels) {
        return std::unexpected(std::string("Could not decode the image: ") + stbi_failure_reason());
    }
    return out;
}

std::expected<std::string, std::string> to_ascii(const std::filesystem::path& path, std::size_t max_cols,
                                                 std::size_t max_rows, int texture) {
    const auto data = read_image_file(path);
    if (!data) {
        return std::unexpected(data.error());
    }
    return to_ascii_data(*data, max_cols, max_rows, texture);
}

std::expected<std::string, std::string> to_ascii_data(std::string_view encoded, std::size_t max_cols,
                                                      std::size_t max_rows, int texture) {
    texture = std::clamp(texture, 0, 100);
    auto decoded = decode_image(encoded);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto& pixels = decoded->pixels;
    const int w = decoded->width;
    const int h = decoded->height;

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
    // How bright the color of each code is (1 for the fixed hues; palette colors can be darker).
    std::vector<double> code_values(rows * cols, 1.0);
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
        const double v = code_values[i];
        const auto step =
            std::clamp<std::size_t>(static_cast<std::size_t>(std::ceil(std::min(1.0, cells[i] / v) * last)), 1, ramp.size() - 1);
        const double wanted = std::min(1.0, cells[i] * last / static_cast<double>(step) / v);
        // The nearest level, or the one already set when it is close enough, which saves a code.
        std::size_t dim = state.dim;
        if (std::abs(dim_levels[dim].level - wanted) > keep_level_within) {
            dim = static_cast<std::size_t>(std::ranges::min_element(dim_levels, {}, [&](const DimLevel& l) {
                                               return std::abs(l.level - wanted);
                                           }) -
                                           dim_levels.begin());
        }
        const Choice uniform {ramp_char(step), dim};
        const double uniform_error = spread(dim_levels[dim].level * static_cast<double>(step) / last * v);

        // Bright and dark parts: quadrants, the lit quarters in the color at some level, the others dark, when they
        // look clearly better. (15, all lit, is an even block.)
        Choice quadrant;
        double quadrant_error = std::numeric_limits<double>::infinity();
        for (std::size_t lit = 1; lit < 15; ++lit) {
            for (std::size_t d = 0; d < dim_levels.size(); ++d) {
                double error = 0;
                for (std::size_t k = 0; k < 4; ++k) {
                    error += std::abs((lit >> k & 1 ? dim_levels[d].level * v : 0.0) - q[k]);
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
    using Rgb = std::array<double, 3>;
    using Points = std::array<Rgb, 64>;
    std::vector<Points> cell_points(rows * cols);
    for (std::size_t r = 0; r < rows; ++r) {
        const std::size_t y0 = r * height / rows;
        const std::size_t y1 = std::max(y0 + 1, (r + 1) * height / rows);
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t x0 = c * width / cols;
            const std::size_t x1 = std::max(x0 + 1, (c + 1) * width / cols);
            Points& points = cell_points[r * cols + c];
            for (std::size_t py = 0; py < 8; ++py) {
                const std::size_t ya = y0 + py * (y1 - y0) / 8;
                const std::size_t yb = std::max(ya + 1, y0 + (py + 1) * (y1 - y0) / 8);
                for (std::size_t px = 0; px < 8; ++px) {
                    const std::size_t xa = x0 + px * (x1 - x0) / 8;
                    const std::size_t xb = std::max(xa + 1, x0 + (px + 1) * (x1 - x0) / 8);
                    Rgb& point = points[py * 8 + px];
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
        }
    }

    // Texture is for the parts that are dark for this picture: in a picture that is dark all over, sparse symbols
    // would make it darker still, so the bar is lowered with how bright its brightest parts are (the 95th percentile
    // of the luminance of its characters, so a few bright spots do not count).
    double texture_threshold = texture_below(texture);
    {
        std::vector<double> luminances;
        for (const Points& points : cell_points) {
            double sum = 0;
            for (const Rgb& p : points) {
                sum += 0.3 * p[0] + 0.59 * p[1] + 0.11 * p[2];
            }
            luminances.push_back(sum / 64);
        }
        if (!luminances.empty()) {
            const auto nth = luminances.begin() + static_cast<std::ptrdiff_t>(luminances.size() * 95 / 100);
            std::ranges::nth_element(luminances, nth);
            const double dark_picture = std::clamp(*nth / bright_luminance, 0.0, 1.0);
            texture_threshold *= dark_picture + (1 - dark_picture) * texture / 100.0;
        }
    }

    const auto distance = [](const Rgb& a, const Rgb& b) {
        return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
    };
    // The colors of this picture: the palette the color codes refer to, sent before the drawing.
    const std::vector<Rgb> palette = make_palette(cell_points);

    struct Paint {
        char code = 0;
        // Index in dim_levels, or nullopt for black.
        std::optional<std::size_t> dim;
        bool operator==(const Paint&) const = default;
    };
    // The palette color at some brightness closest to a color, and the color it gives.
    const auto paint_of = [&](const Rgb& rgb) {
        Paint best;
        Rgb shown {};
        double best_distance = distance(rgb, shown);
        for (std::size_t p = 0; p < palette.size(); ++p) {
            for (std::size_t d = 0; d < dim_levels.size(); ++d) {
                const double l = dim_levels[d].level;
                const Rgb color {palette[p][0] * l, palette[p][1] * l, palette[p][2] * l};
                if (const double e = distance(rgb, color); e < best_distance) {
                    best_distance = e;
                    best = {color_codes[p], d};
                    shown = color;
                }
            }
        }
        return std::pair {best, shown};
    };

    const auto two_color_row = [&](std::size_t r, double margin) {
        std::string line;
        // Each row starts over without a color, at full brightness and without a background.
        char code = 0;
        std::size_t dim = full_level;
        bool black = false;
        bool bold = false;
        std::optional<Paint> background;
        for (std::size_t c = 0; c < cols; ++c) {
            const Points& points = cell_points[r * cols + c];
            // Each candidate is scored by how far its colors are from the points, as the sum of the absolute
            // differences: unlike their squares, it does not favor averaging a thin line with what is around it into
            // a muddy color over keeping it crisp, a little off its place.
            Rgb total {};
            for (const Rgb& p : points) {
                for (std::size_t k = 0; k < 3; ++k) {
                    total[k] += p[k];
                }
            }
            const auto error_of = [&](std::uint64_t mask, const Rgb& ink, const Rgb& rest) {
                double error = 0;
                for (std::size_t b = 0; b < 64; ++b) {
                    const Rgb& q = mask >> b & 1 ? ink : rest;
                    error += std::abs(points[b][0] - q[0]) + std::abs(points[b][1] - q[1]) + std::abs(points[b][2] - q[2]);
                }
                return error;
            };
            const auto [even_paint, even_shown] = paint_of({total[0] / 64, total[1] / 64, total[2] / 64});
            double best_error = error_of(0, even_shown, even_shown);
            const Block* best = nullptr;
            Paint fg = even_paint;
            Paint bg;
            const auto consider = [&](const Block& block, const std::pair<Paint, Rgb>& ink, const std::pair<Paint, Rgb>& rest) {
                if (ink.first == rest.first) {
                    return;
                }
                if (const double error = error_of(block.mask, ink.second, rest.second) + margin; error < best_error) {
                    best_error = error;
                    best = &block;
                    fg = ink.first;
                    bg = rest.first;
                }
            };

            // The two main colors of the character, which keep a white and a black crisp where the averages of a
            // block's parts would mix them: 2-means, from its darkest and brightest points.
            const auto luma = [](const Rgb& p) {
                return 0.3 * p[0] + 0.59 * p[1] + 0.11 * p[2];
            };
            std::array<Rgb, 2> centers {*std::ranges::min_element(points, {}, luma), *std::ranges::max_element(points, {}, luma)};
            for (int iteration = 0; iteration < 4; ++iteration) {
                std::array<Rgb, 2> sums {};
                std::array<double, 2> counts {};
                for (const Rgb& p : points) {
                    const std::size_t j = distance(p, centers[0]) <= distance(p, centers[1]) ? 0 : 1;
                    for (std::size_t k = 0; k < 3; ++k) {
                        sums[j][k] += p[k];
                    }
                    counts[j] += 1;
                }
                for (std::size_t j = 0; j < 2; ++j) {
                    if (counts[j] > 0) {
                        centers[j] = {sums[j][0] / counts[j], sums[j][1] / counts[j], sums[j][2] / counts[j]};
                    }
                }
            }
            const auto dark = paint_of(centers[0]);
            const auto bright = paint_of(centers[1]);

            for (const auto& block : blocks) {
                // The averages of the block's two parts.
                Rgb in {};
                for (std::size_t b = 0; b < 64; ++b) {
                    if (block.mask >> b & 1) {
                        for (std::size_t k = 0; k < 3; ++k) {
                            in[k] += points[b][k];
                        }
                    }
                }
                const auto n_in = static_cast<double>(std::popcount(block.mask));
                const Rgb out {total[0] - in[0], total[1] - in[1], total[2] - in[2]};
                consider(block, paint_of({in[0] / n_in, in[1] / n_in, in[2] / n_in}),
                         paint_of({out[0] / (64 - n_in), out[1] / (64 - n_in), out[2] / (64 - n_in)}));
                // The character's two main colors, either way around.
                consider(block, dark, bright);
                consider(block, bright, dark);
            }

            std::string_view glyph = "@"; // an even character: '@', a full block when shown (see render())
            if (best) {
                glyph = best->glyph;
            }
            if ((!best && !fg.dim) || (best && !fg.dim && !bg.dim)) {
                glyph = " ";
            }
            // A dark character is drawn as text instead of blocks: a symbol with just enough ink, on black, in the
            // color that keeps the light of the character the same. The texture sets how dark (see texture_below()):
            // as it grows, more and more of the dark range becomes text, while what is brighter keeps its blocks,
            // which draw corners and thin lines that symbols cannot.
            //
            // How dark a character is: for one that is mostly black, with a few lit bits (like the shadows of a
            // drawing, which symbols on black look like), its average; for any other, its brightest color, so that
            // an edge between a dark part and a bright one keeps its blocks, and its sharpness.
            const auto black_points = std::ranges::count_if(points, [](const Rgb& p) {
                return 0.3 * p[0] + 0.59 * p[1] + 0.11 * p[2] < near_black;
            });
            // Dark as it looks (luminance: a deep red is dark), as long as symbols can still give its strongest
            // channel enough light, or it would come out dimmer than it is.
            const auto luminance = [](const Rgb& c) {
                return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
            };
            const auto color_of = [&](const Paint& p) {
                if (!p.dim) {
                    return Rgb {};
                }
                const Rgb& color = palette[color_codes.find(p.code)];
                const double l = dim_levels[*p.dim].level;
                return Rgb {color[0] * l, color[1] * l, color[2] * l};
            };
            const Rgb mean {total[0] / 64, total[1] / 64, total[2] / 64};
            const Rgb brighter = !best ? mean : luminance(color_of(fg)) >= luminance(color_of(bg)) ? color_of(fg) : color_of(bg);
            const Rgb& judged = black_points >= 32 || !best ? mean : brighter;
            const bool reachable = std::max({judged[0], judged[1], judged[2]}) <= texture_below(100) * reach_tolerance;
            bool textured = false;
            bool textured_bold = false;
            if (glyph != " " && luminance(judged) < texture_threshold && reachable &&
                black_points >= min_black_points(texture)) {
                const Rgb light {to_light(total[0] / 64), to_light(total[1] / 64), to_light(total[2] / 64)};
                const double needed = std::max({light[0], light[1], light[2]});
                const Texture* symbol = nullptr;
                for (const Texture& t : textures) {
                    symbol = &t;
                    if (t.ink * to_light(texture_level) >= needed) {
                        break;
                    }
                }
                // When even the densest symbol cannot give that much light, the color is as bright as it gets with
                // its hue kept (all channels scaled together, not each capped, which would whiten it).
                const double over = std::max(1.0, needed / symbol->ink);
                const Rgb color {to_value(light[0] / symbol->ink / over), to_value(light[1] / symbol->ink / over),
                                 to_value(light[2] / symbol->ink / over)};
                if (const Paint paint = paint_of(color).first; paint.dim) {
                    fg = paint;
                    glyph = symbol->glyph;
                    textured = true;
                    textured_bold = symbol->bold;
                }
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
                if (fg.code != code) {
                    code = fg.code;
                    line += code;
                }
                if (black || *fg.dim != dim) {
                    dim = *fg.dim;
                    black = false;
                    line += dim_levels[dim].code;
                }
            }
            if (textured) {
                // Texture is drawn on black; a symbol after the literal code is shown as itself, not as a block.
                if (background) {
                    line += no_background_code;
                    background.reset();
                }
                if (glyph.size() == 1) {
                    if (textured_bold != bold) {
                        bold = textured_bold;
                        line += bold_code;
                    }
                    line += literal_code;
                }
                line += glyph;
                continue;
            }
            // A full character hides its background, which then stays as it is.
            if (best) {
                const std::optional<Paint> wanted = bg.dim ? std::optional(bg) : std::nullopt;
                if (wanted != background) {
                    if (wanted) {
                        line += background_code;
                        line += wanted->code;
                        line += dim_levels[*wanted->dim].code;
                    } else {
                        line += no_background_code;
                    }
                    background = wanted;
                }
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

    // The simpler ways (see Style) take the palette color with the hue closest to each character's.
    for (std::size_t i = 0; i < codes.size(); ++i) {
        Rgb mean {};
        for (const Rgb& p : cell_points[i]) {
            for (std::size_t k = 0; k < 3; ++k) {
                mean[k] += p[k] / 64;
            }
        }
        const auto normalized = [](Rgb v) {
            const double m = std::max({v[0], v[1], v[2], 1e-6});
            return Rgb {v[0] / m, v[1] / m, v[2] / m};
        };
        std::size_t best = 0;
        for (std::size_t p = 1; p < palette.size(); ++p) {
            if (distance(normalized(mean), normalized(palette[p])) < distance(normalized(mean), normalized(palette[best]))) {
                best = p;
            }
        }
        codes[i] = palette.empty() ? 0 : color_codes[best];
        code_values[i] = palette.empty() ? 1.0 : std::max({palette[best][0], palette[best][1], palette[best][2], 0.05});
    }

    std::vector<std::string> lines(rows);
    for (std::size_t r = 0; r < rows; ++r) {
        // A row with a lot of detail can be too long for a packet: then only its clearest details keep their
        // blocks, the others get a single color, until it fits.
        bool fits = false;
        for (double margin = detail_margin; margin <= detail_margin * 256 && !fits; margin *= 2) {
            lines[r] = two_color_row(r, margin);
            fits = lines[r].size() <= max_art_row_bytes;
        }
        if (fits) {
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
    // The palette first (see palette_code), then the drawing.
    std::string art(1, palette_code);
    const double top = static_cast<double>(palette_digits.size() - 1);
    for (const Rgb& color : palette) {
        for (const double v : color) {
            art += palette_digits[static_cast<std::size_t>(std::lround(v * top))];
        }
    }
    for (const auto& line : lines) {
        art += "\n";
        art += line.empty() ? line : line.substr(indent);
    }
    return art;
}

namespace {

    constexpr std::string_view base64_digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string base64_encode(std::string_view in) {
        std::string out;
        out.reserve((in.size() + 2) / 3 * 4);
        std::size_t i = 0;
        for (; i + 2 < in.size(); i += 3) {
            const auto v = static_cast<unsigned>(static_cast<unsigned char>(in[i])) << 16 |
                           static_cast<unsigned>(static_cast<unsigned char>(in[i + 1])) << 8 |
                           static_cast<unsigned char>(in[i + 2]);
            out += base64_digits[v >> 18 & 63];
            out += base64_digits[v >> 12 & 63];
            out += base64_digits[v >> 6 & 63];
            out += base64_digits[v & 63];
        }
        if (i < in.size()) {
            auto v = static_cast<unsigned>(static_cast<unsigned char>(in[i])) << 16;
            if (i + 1 < in.size()) {
                v |= static_cast<unsigned>(static_cast<unsigned char>(in[i + 1])) << 8;
            }
            out += base64_digits[v >> 18 & 63];
            out += base64_digits[v >> 12 & 63];
            out += i + 1 < in.size() ? base64_digits[v >> 6 & 63] : '=';
            out += '=';
        }
        return out;
    }

    std::string base64_decode(std::string_view in) {
        std::string out;
        unsigned value = 0;
        int bits = 0;
        for (const char c : in) {
            const auto d = base64_digits.find(c);
            if (d == std::string_view::npos) {
                continue;
            }
            value = (value << 6) | static_cast<unsigned>(d);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out += static_cast<char>((value >> bits) & 0xFF);
            }
        }
        return out;
    }

    // An image (RGBA, sw x sh) at a smaller size, RGB on black: each pixel the average of the ones it covers.
    std::vector<unsigned char> shrink(const unsigned char* rgba, int sw, int sh, int w, int h) {
        std::vector<unsigned char> out(static_cast<std::size_t>(w) * h * 3);
        for (int y = 0; y < h; ++y) {
            const int y0 = y * sh / h;
            const int y1 = std::max(y0 + 1, (y + 1) * sh / h);
            for (int x = 0; x < w; ++x) {
                const int x0 = x * sw / w;
                const int x1 = std::max(x0 + 1, (x + 1) * sw / w);
                std::array<double, 3> sum {};
                for (int sy = y0; sy < y1; ++sy) {
                    for (int sx = x0; sx < x1; ++sx) {
                        const unsigned char* p = rgba + (static_cast<std::size_t>(sy) * sw + sx) * 4;
                        for (int k = 0; k < 3; ++k) {
                            sum[k] += p[k] * p[3] / 255.0;
                        }
                    }
                }
                const double n = static_cast<double>((y1 - y0) * (x1 - x0));
                for (int k = 0; k < 3; ++k) {
                    out[(static_cast<std::size_t>(y) * w + x) * 3 + k] =
                        static_cast<unsigned char>(std::lround(sum[k] / n));
                }
            }
        }
        return out;
    }

    void append_to_string(void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
    }

    std::string jpeg_of(const std::vector<unsigned char>& rgb, int w, int h, int quality) {
        std::string jpeg;
        stbi_write_jpg_to_func(append_to_string, &jpeg, w, h, 3, rgb.data(), quality);
        return jpeg;
    }

    // At most max_size wide and tall, never bigger than it is.
    std::pair<int, int> fit(int width, int height, int max_size) {
        const double scale = std::min(1.0, static_cast<double>(max_size) / std::max(width, height));
        return {std::max(1, static_cast<int>(std::lround(width * scale))),
                std::max(1, static_cast<int>(std::lround(height * scale)))};
    }

    bool is_gif(std::string_view data) {
        return data.starts_with("GIF87a") || data.starts_with("GIF89a");
    }

    // An animated GIF: its frames (RGBA, one after the other) and how long each is shown, in milliseconds.
    struct Animation {
        std::unique_ptr<unsigned char, void (*)(void*)> pixels {nullptr, &stbi_image_free};
        std::vector<int> delays;
        int width = 0;
        int height = 0;
        int frames = 0;
    };

    // How many frames a GIF has, from its blocks, without decoding it; 0 when it is not a GIF that reads well.
    int gif_frames(std::string_view data) {
        if (!is_gif(data) || data.size() < 13) {
            return 0;
        }
        const auto byte = [&](std::size_t i) -> unsigned char {
            return i < data.size() ? static_cast<unsigned char>(data[i]) : 0;
        };
        const auto skip_sub_blocks = [&](std::size_t i) {
            while (i < data.size() && byte(i) != 0) {
                i += byte(i) + 1;
            }
            return i + 1;
        };
        std::size_t i = 13;
        if (byte(10) & 0x80) {
            i += 3 * (std::size_t {2} << (byte(10) & 7));
        }
        int frames = 0;
        while (i < data.size()) {
            if (byte(i) == 0x21) {
                i = skip_sub_blocks(i + 2);
            } else if (byte(i) == 0x2C) {
                ++frames;
                const unsigned char flags = byte(i + 9);
                i += 10;
                if (flags & 0x80) {
                    i += 3 * (std::size_t {2} << (flags & 7));
                }
                i = skip_sub_blocks(i + 1);
            } else {
                break;
            }
        }
        return frames;
    }

    std::optional<Animation> decode_animation(std::string_view data) {
        if (!is_gif(data)) {
            return std::nullopt;
        }
        Animation a;
        int* delays = nullptr;
        int channels = 0;
        a.pixels.reset(stbi_load_gif_from_memory(reinterpret_cast<const stbi_uc*>(data.data()),
                                                 static_cast<int>(data.size()), &delays, &a.width, &a.height,
                                                 &a.frames, &channels, 4));
        if (!a.pixels || a.frames <= 0 || static_cast<long long>(a.width) * a.height * a.frames > max_pixels) {
            STBI_FREE(delays);
            return std::nullopt;
        }
        for (int i = 0; i < a.frames; ++i) {
            // Browsers show frames without a delay (or a very short one) for a tenth of a second.
            a.delays.push_back(delays && delays[i] >= 20 ? delays[i] : 100);
        }
        STBI_FREE(delays);
        return a;
    }

    // An animation made of JPEG frames: "anim W H", then a line per frame, "DELAY BASE64". Every frame is kept, at the
    // best quality and size that fit; frames are skipped (each kept one shown for as long as the ones it stands for,
    // so it plays at its speed) only when nothing else does.
    std::optional<std::string> encode_animation(const Animation& a, int max_size) {
        const std::size_t frame_bytes = static_cast<std::size_t>(a.width) * a.height * 4;
        for (const int size : {max_size, 640, 480, 320, 240, 160}) {
            if (size > max_size) {
                continue;
            }
            const auto [w, h] = fit(a.width, a.height, size);
            std::vector<std::vector<unsigned char>> frames;
            for (int i = 0; i < a.frames; ++i) {
                frames.push_back(shrink(a.pixels.get() + frame_bytes * i, a.width, a.height, w, h));
            }
            for (const int quality : {90, 75, 60}) {
                std::vector<std::string> encoded;
                for (const auto& frame : frames) {
                    encoded.push_back(base64_encode(jpeg_of(frame, w, h, quality)));
                }
                const int max_step = size == 160 && quality == 60 ? a.frames : 1;
                for (int step = 1; step <= max_step; ++step) {
                    std::string text = std::format("anim {} {}", w, h);
                    for (int i = 0; i < a.frames; i += step) {
                        int delay = 0;
                        for (int j = i; j < std::min(a.frames, i + step); ++j) {
                            delay += a.delays[j];
                        }
                        text += std::format("\n{} ", delay);
                        text += encoded[i];
                        if (text.size() > max_image_bytes) {
                            break;
                        }
                    }
                    if (text.size() <= max_image_bytes) {
                        return text;
                    }
                }
            }
        }
        return std::nullopt;
    }

    // The kind of picture file windows show as it is, from its first bytes; empty for others.
    std::string_view file_kind(std::string_view data) {
        if (is_gif(data)) {
            return "gif";
        }
        if (data.starts_with("\x89PNG\r\n\x1a\n")) {
            return "png";
        }
        if (data.starts_with("\xFF\xD8\xFF")) {
            return "jpeg";
        }
        if (data.starts_with("BM")) {
            return "bmp";
        }
        return {};
    }

} // namespace

std::expected<std::string, std::string> encode_picture(const std::filesystem::path& path, int max_size,
                                                       bool* still_of_animation) {
    if (still_of_animation) {
        *still_of_animation = false;
    }
    const auto data = read_image_file(path);
    if (!data) {
        return std::unexpected(data.error());
    }
    const auto image = decode_image(*data);
    if (!image) {
        return std::unexpected(image.error());
    }
    const auto [show_w, show_h] = fit(image->width, image->height, max_size);
    // The file as it is, whenever it can be: at its best, and an animated GIF plays at its own speed, as any GIF
    // does. The size is only the one it is shown at; opened big, it shows all of it.
    if (const auto kind = file_kind(*data); !kind.empty()) {
        std::string text = std::format("{} {} {}\n", kind, show_w, show_h);
        text += base64_encode(*data);
        if (text.size() <= max_image_bytes) {
            return text;
        }
    }
    // Otherwise made again, as big and good as fits. An animated GIF as JPEG frames; one with too many pixels in all
    // its frames to decode is sent as its first frame.
    const int frames = gif_frames(*data);
    const bool too_long = frames > 1 && static_cast<long long>(image->width) * image->height * frames > max_pixels;
    if (too_long && still_of_animation) {
        *still_of_animation = true;
    }
    if (const auto animation = frames > 1 && !too_long ? decode_animation(*data) : std::nullopt;
        animation && animation->frames > 1) {
        if (auto text = encode_animation(*animation, std::max(max_size, max_remade_size))) {
            return std::move(*text);
        }
        return std::unexpected("That animation cannot be made small enough to send.");
    }
    auto [w, h] = fit(image->width, image->height, std::max(max_size, max_remade_size));
    // Lower quality first, then a smaller picture, until it fits.
    while (true) {
        const auto rgb = shrink(image->pixels.get(), image->width, image->height, w, h);
        for (const int quality : {92, 80, 65}) {
            const std::string jpeg = jpeg_of(rgb, w, h, quality);
            std::string text = std::format("jpeg {} {}\n", show_w, show_h);
            text += base64_encode(jpeg);
            if (!jpeg.empty() && text.size() <= max_image_bytes) {
                return text;
            }
        }
        if (w < 32 || h < 32) {
            return std::unexpected("That image cannot be made small enough to send.");
        }
        w = w * 4 / 5;
        h = h * 4 / 5;
    }
}

namespace {

    // The middle square of an RGBA image.
    std::vector<unsigned char> crop_square(const unsigned char* rgba, int w, int h) {
        const int side = std::min(w, h);
        const int x0 = (w - side) / 2;
        const int y0 = (h - side) / 2;
        std::vector<unsigned char> out(static_cast<std::size_t>(side) * side * 4);
        for (int y = 0; y < side; ++y) {
            std::copy_n(rgba + (static_cast<std::size_t>(y0 + y) * w + x0) * 4, static_cast<std::size_t>(side) * 4,
                        out.data() + static_cast<std::size_t>(y) * side * 4);
        }
        return out;
    }

    constexpr std::size_t max_avatar_gif_bytes = 2 * 1024 * 1024;

} // namespace

std::expected<std::string, std::string> encode_avatar(const std::filesystem::path& path) {
    const auto data = read_image_file(path);
    if (!data) {
        return std::unexpected(data.error());
    }
    const auto image = decode_image(*data);
    if (!image) {
        return std::unexpected(image.error());
    }
    const int frames = gif_frames(*data);
    if (frames > 1) {
        // As it is: it plays by itself, at its best.
        if (data->size() <= max_avatar_gif_bytes) {
            std::string text = "gif 128 128\n";
            text += base64_encode(*data);
            if (text.size() <= max_avatar_bytes) {
                return text;
            }
        }
        // Or square JPEG frames, all of them, smaller until they fit.
        const bool too_long = static_cast<long long>(image->width) * image->height * frames > max_pixels;
        if (const auto a = too_long ? std::nullopt : decode_animation(*data); a && a->frames > 1) {
            const int side = std::min(a->width, a->height);
            const std::size_t frame_bytes = static_cast<std::size_t>(a->width) * a->height * 4;
            std::vector<std::vector<unsigned char>> squares;
            for (int i = 0; i < a->frames; ++i) {
                squares.push_back(crop_square(a->pixels.get() + frame_bytes * i, a->width, a->height));
            }
            for (const int size : {128, 96, 64}) {
                const int s = std::min(size, side);
                std::string text = std::format("anim {} {}", s, s);
                for (int i = 0; i < a->frames && text.size() <= max_avatar_bytes; ++i) {
                    text += std::format("\n{} ", a->delays[i]);
                    text += base64_encode(jpeg_of(shrink(squares[i].data(), side, side, s, s), s, s, 80));
                }
                if (text.size() <= max_avatar_bytes) {
                    return text;
                }
            }
        }
        // Too much to animate: its first frame, as below.
    }
    const int side = std::min(image->width, image->height);
    const int s = std::min(256, side);
    const auto square = crop_square(image->pixels.get(), image->width, image->height);
    std::string text = std::format("jpeg {} {}\n", s, s);
    text += base64_encode(jpeg_of(shrink(square.data(), side, side, s, s), s, s, 90));
    return text;
}

std::optional<Picture> parse_picture(std::string_view text) {
    auto nl = text.find('\n');
    if (nl == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view head = text.substr(0, nl);
    const auto kind_end = head.find(' ');
    if (kind_end == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view kind = head.substr(0, kind_end);
    const std::string_view size = head.substr(kind_end + 1);
    const auto space = size.find(' ');
    if (space == std::string_view::npos) {
        return std::nullopt;
    }
    const auto number = [](std::string_view s, int& v, int max) {
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        return ec == std::errc {} && ptr == s.data() + s.size() && v > 0 && v <= max;
    };
    Picture picture;
    if (!number(size.substr(0, space), picture.width, 4096) || !number(size.substr(space + 1), picture.height, 4096)) {
        return std::nullopt;
    }
    std::string_view body = text.substr(nl + 1);
    if (kind == "jpeg" || kind == "gif" || kind == "png" || kind == "bmp") {
        picture.frames.push_back({std::format("image/{}", kind), std::string(body), 0});
    } else if (kind == "anim") {
        // A line per frame: "DELAY BASE64".
        while (!body.empty()) {
            nl = body.find('\n');
            const std::string_view line = body.substr(0, nl);
            body.remove_prefix(nl == std::string_view::npos ? body.size() : nl + 1);
            const auto gap = line.find(' ');
            int delay = 0;
            if (gap == std::string_view::npos || !number(line.substr(0, gap), delay, 60000)) {
                continue;
            }
            picture.frames.push_back({"image/jpeg", std::string(line.substr(gap + 1)), delay});
        }
    } else {
        return std::nullopt;
    }
    if (picture.frames.empty()) {
        return std::nullopt;
    }
    // The first frame, for where it is drawn with characters.
    picture.first = base64_decode(picture.frames.front().base64);
    if (picture.first.empty()) {
        return std::nullopt;
    }
    return picture;
}

std::string render(std::string_view art, bool colors) {
    // Each character becomes a solid block of its color, as dark as its glyph is thin: the picture looks the same
    // in every terminal, instead of depending on how its font draws the symbols and how much space it leaves between
    // lines. Only the way it is shown changes, so pictures from older versions look the same. The brightness codes
    // dim it further, and the blocks keep their shape, in their color on their background color. Without colors,
    // each character becomes the ASCII symbol as bright as it looks, which gives the black and white ASCII art.
    std::string out;
    // A palette row first (see palette_code): the color codes are then its colors, instead of the fixed hues.
    std::vector<Color> palette;
    if (art.starts_with(palette_code)) {
        const auto end = art.find('\n');
        const std::string_view row = art.substr(1, end == std::string_view::npos ? std::string_view::npos : end - 1);
        const auto channel = [](char digit) {
            const auto v = palette_digits.find(digit);
            return static_cast<std::uint8_t>(v == std::string_view::npos ? 0 : v * 255 / (palette_digits.size() - 1));
        };
        for (std::size_t i = 0; i + 2 < row.size(); i += 3) {
            palette.push_back({channel(row[i]), channel(row[i + 1]), channel(row[i + 2])});
        }
        art = end == std::string_view::npos ? std::string_view() : art.substr(end + 1);
    }
    const auto is_code = [&](char c) {
        return palette.empty() ? color_of_code(c).has_value() : color_codes.find(c) != std::string_view::npos;
    };
    const auto color_of = [&](char c) -> std::optional<Color> {
        if (palette.empty()) {
            return color_of_code(c);
        }
        const auto i = color_codes.find(c);
        return i < palette.size() ? std::optional(palette[i]) : std::nullopt;
    };
    std::optional<Color> hue;
    double level = 1.0;
    std::optional<Color> background;
    std::optional<Color> shown;
    std::optional<Color> shown_background;
    bool reading_background = false;
    // Bold is only for texture symbols (see bold_code): some terminals show bold text in a brighter color.
    bool bold = false;
    bool shown_bold = false;
    const auto set_bold = [&](bool on) {
        if (colors && on != shown_bold) {
            out += on ? "\x1b[1m" : "\x1b[22m";
            shown_bold = on;
        }
    };
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
            if (const auto color = color_of(c); color && i + 1 < art.size()) {
                background = scaled(*color, dim_of_code(art[i + 1]).value_or(1.0));
                ++i;
            }
            reading_background = false;
            continue;
        }
        if (is_code(c)) {
            hue = color_of(c);
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
        if (c == bold_code) {
            bold = !bold;
            continue;
        }
        if (c == literal_code && i + 1 < art.size() && art[i + 1] != '\n') {
            // Texture: the symbol as itself, in its color, on black.
            ++i;
            if (colors) {
                show(level, false);
                set_bold(bold);
            }
            out += art[i];
            continue;
        }
        // Anything else is never bold.
        set_bold(false);
        if (c == '\n') {
            // Each row starts over without a color, at full brightness, without a background and not bold, like in
            // to_ascii().
            if (colors && (shown || shown_background)) {
                out += "\x1b[0m";
            }
            hue.reset();
            background.reset();
            shown.reset();
            shown_background.reset();
            level = 1.0;
            bold = false;
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
