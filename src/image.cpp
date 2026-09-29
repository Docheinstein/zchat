#include "image.hpp"

#include "color.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
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

    // From dark to bright: a bright pixel gets a character with more ink, as terminals are usually dark.
    constexpr std::string_view ramp = " .:-=+*#%@";

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
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
        raw = s.substr(1, s.size() - 2);
    } else if (s.starts_with("file://")) {
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
        // Backslashes separate folders on Windows.
        raw = s;
#else
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                ++i;
            }
            raw += s[i];
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

    // The average brightness and color of the pixels under each character.
    std::vector<double> cells(rows * cols);
    std::vector<char> codes(rows * cols);
    for (std::size_t r = 0; r < rows; ++r) {
        const std::size_t y0 = r * height / rows;
        const std::size_t y1 = std::max(y0 + 1, (r + 1) * height / rows);
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t x0 = c * width / cols;
            const std::size_t x1 = std::max(x0 + 1, (c + 1) * width / cols);
            double sum = 0;
            std::array<double, 3> rgb {};
            for (std::size_t y = y0; y < y1; ++y) {
                for (std::size_t x = x0; x < x1; ++x) {
                    const unsigned char* pixel = pixels.get() + (y * width + x) * 4;
                    sum += brightness(pixel);
                    for (std::size_t k = 0; k < 3; ++k) {
                        rgb[k] += static_cast<double>(pixel[k]) * pixel[3];
                    }
                }
            }
            cells[r * cols + c] = sum / static_cast<double>((y1 - y0) * (x1 - x0));
            codes[r * cols + c] = color_code(rgb);
        }
    }

    // Stretch the contrast, so dim or washed out pictures use the whole ramp.
    const auto [lo, hi] = std::ranges::minmax(cells);
    if (hi - lo > 0.05) {
        for (double& v : cells) {
            v = (v - lo) / (hi - lo);
        }
    }

    std::vector<std::string> lines(rows);
    for (std::size_t r = 0; r < rows; ++r) {
        // Each row starts over without a color, so it can be read by itself.
        char color = 0;
        for (std::size_t c = 0; c < cols; ++c) {
            const auto level = static_cast<std::size_t>(cells[r * cols + c] * static_cast<double>(ramp.size()));
            const char glyph = ramp[std::min(level, ramp.size() - 1)];
            // A color only when it changes, and never for spaces, which do not show it.
            if (glyph != ' ' && codes[r * cols + c] != color) {
                color = codes[r * cols + c];
                lines[r] += color;
            }
            lines[r] += glyph;
        }
        lines[r].erase(lines[r].find_last_not_of(' ') + 1);
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
    std::string out;
    bool colored = false;
    for (const char c : art) {
        if (const auto color = color_of_code(c)) {
            if (colors) {
                out += std::format("\x1b[{}m", ansi_foreground(*color));
                colored = true;
            }
            continue;
        }
        if (c == '\n' && colored) {
            out += "\x1b[0m";
            colored = false;
        }
        out += c;
    }
    if (colored) {
        out += "\x1b[0m";
    }
    return out;
}

} // namespace zchat::image
