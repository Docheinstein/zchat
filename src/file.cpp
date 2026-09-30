#include "file.hpp"

#include "image.hpp"
#include "protocol.hpp"
#include "sound.hpp"
#include "text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <shlobj.h>
#else
#include <cstdlib>
#endif

namespace zchat::file {

namespace {

    constexpr std::string_view file_kind = "file ";
    constexpr std::string_view trill_kind = "trill ";
    // The name is in base64 too, as the pieces of a big file keep nothing else (see sanitize_text() in protocol.cpp):
    // with "file" and the size, it fits in the first line of an Image. A trill's two names are shorter, to fit too.
    static_assert(file_kind.size() + 21 + (max_name_bytes + 2) / 3 * 4 <= max_image_head_bytes);
    constexpr std::size_t max_trill_name_bytes = 64;
    static_assert(trill_kind.size() + 21 + 2 * ((max_trill_name_bytes + 2) / 3 * 4 + 1) <= max_image_head_bytes);

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
        out.reserve(in.size() / 4 * 3);
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

    std::string lowercase(std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        return out;
    }

    std::filesystem::path to_path(std::string_view utf8) {
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    }

} // namespace

std::uintmax_t max_bytes() {
    return (max_image_bytes - max_image_head_bytes) / 4 * 3;
}

bool is_dropped_file(std::string_view line) {
    const std::filesystem::path path = image::parse_path(line);
    // Only a full path: a word typed in the chat could be the name of a file in the current folder.
    std::error_code ec;
    return !path.empty() && path.is_absolute() && std::filesystem::is_regular_file(path, ec);
}

std::expected<std::string, std::string> read(const std::filesystem::path& path, std::uintmax_t limit) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return std::unexpected("That is a folder: only files can be sent.");
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected("There is no such file.");
    }
    if (size > limit) {
        return std::unexpected(std::format("That file is too big: up to {} can be sent.", format_size(limit)));
    }
    std::ifstream in(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if ((!in && !in.eof()) || data.size() != size) {
        return std::unexpected("Could not read the file.");
    }
    return data;
}

std::expected<std::string, std::string> encode(const std::filesystem::path& path) {
    const auto data = read(path, max_bytes());
    if (!data) {
        return std::unexpected(data.error());
    }
    const auto u8 = path.filename().u8string();
    const std::string name = safe_name(std::string_view(reinterpret_cast<const char*>(u8.data()), u8.size()));
    std::string text = std::format("{}{} {}\n", file_kind, data->size(), base64_encode(name));
    text += base64_encode(*data);
    return text;
}

bool is_file(std::string_view text) {
    return text.starts_with(file_kind);
}

bool is_trill(std::string_view text) {
    return text.starts_with(trill_kind);
}

std::string encode_trill(const Trill& trill) {
    const auto name = [](std::string_view data, std::string_view name) {
        return data.empty() ? std::string("-") : base64_encode(safe_name(name, max_trill_name_bytes));
    };
    std::string text = std::format("{}{} {} {}\n", trill_kind, trill.sound.size(), name(trill.sound, trill.sound_name),
                                   name(trill.picture, trill.picture_name));
    text += base64_encode(trill.sound);
    text += '\n';
    text += trill.picture;
    return text;
}

std::optional<Trill> parse_trill(std::string_view text) {
    if (!is_trill(text)) {
        return std::nullopt;
    }
    const auto nl = text.find('\n');
    if (nl == std::string_view::npos) {
        return std::nullopt;
    }
    // "SIZE SOUND_NAME PICTURE_NAME", or "SIZE NAME" from before pictures.
    std::vector<std::string_view> fields;
    for (auto head = text.substr(trill_kind.size(), nl - trill_kind.size()); !head.empty();) {
        const auto space = head.find(' ');
        fields.push_back(head.substr(0, space));
        head.remove_prefix(space == std::string_view::npos ? head.size() : space + 1);
    }
    if (fields.size() != 2 && fields.size() != 3) {
        return std::nullopt;
    }
    std::uintmax_t size = 0;
    const std::string_view number = fields[0];
    if (const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), size);
        ec != std::errc {} || ptr != number.data() + number.size() || size > sound::max_bytes) {
        return std::nullopt;
    }
    const std::string_view rest = text.substr(nl + 1);
    const auto end = rest.find('\n');
    Trill trill;
    if (size > 0) {
        trill.sound = base64_decode(rest.substr(0, end));
        trill.sound_name = safe_name(base64_decode(fields[1]), max_trill_name_bytes);
        // One cut short (or padded) is not the sound that was sent, and players are only ever given sounds.
        if (trill.sound.size() != size || !sound::is_audio(trill.sound)) {
            return std::nullopt;
        }
    }
    if (fields.size() == 3 && fields[2] != "-" && end != std::string_view::npos) {
        trill.picture = rest.substr(end + 1);
        trill.picture_name = safe_name(base64_decode(fields[2]), max_trill_name_bytes);
        if (trill.picture.size() > max_trill_picture_bytes || !image::parse_picture(trill.picture)) {
            return std::nullopt;
        }
    }
    if (trill.sound.empty() && trill.picture.empty()) {
        return std::nullopt;
    }
    return trill;
}

std::optional<Received> parse(std::string_view text) {
    if (!is_file(text)) {
        return std::nullopt;
    }
    const auto nl = text.find('\n');
    if (nl == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view head = text.substr(file_kind.size(), nl - file_kind.size());
    const auto space = head.find(' ');
    if (space == std::string_view::npos) {
        return std::nullopt;
    }
    std::uintmax_t size = 0;
    const auto [ptr, ec] = std::from_chars(head.data(), head.data() + space, size);
    if (ec != std::errc {} || ptr != head.data() + space || size > max_bytes()) {
        return std::nullopt;
    }
    Received file {safe_name(base64_decode(head.substr(space + 1))), base64_decode(text.substr(nl + 1))};
    // A file cut short (or padded) is not the file that was sent.
    if (file.data.size() != size) {
        return std::nullopt;
    }
    return file;
}

std::string safe_name(std::string_view name, std::size_t limit) {
    std::string out;
    for (const char c : text::sanitize(name, limit)) {
        const auto u = static_cast<unsigned char>(c);
        const bool fine =
            std::isalnum(u) || u >= 0x80 || std::string_view(".-_ ()[]+,").find(c) != std::string_view::npos;
        out += fine ? c : '_';
    }
    // Windows drops spaces and dots at the ends of names, and a name of dots is a folder.
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) {
        out.erase(out.begin());
    }
    if (out.empty()) {
        return "file";
    }
    // CON, NUL, COM1... are devices on Windows, whatever their extension.
    static constexpr std::array reserved {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
                                          "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
                                          "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    const std::string stem = lowercase(out.substr(0, out.find('.')));
    if (std::ranges::find(reserved, stem) != reserved.end()) {
        out.insert(out.begin(), '_');
    }
    return out;
}

std::string format_size(std::uintmax_t bytes) {
    if (bytes < 1024) {
        return std::format("{} byte{}", bytes, bytes == 1 ? "" : "s");
    }
    const double kb = static_cast<double>(bytes) / 1024;
    if (kb < 1024) {
        return std::format("{:.{}f} KB", kb, kb < 10 ? 1 : 0);
    }
    const double mb = kb / 1024;
    return std::format("{:.{}f} MB", mb, mb < 10 ? 1 : 0);
}

std::filesystem::path downloads_dir() {
    std::error_code ec;
#ifdef _WIN32
    for (const auto& id : {FOLDERID_Downloads, FOLDERID_Profile}) {
        PWSTR folder = nullptr;
        std::filesystem::path dir;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) {
            dir = folder;
        }
        CoTaskMemFree(folder);
        if (!dir.empty() && std::filesystem::is_directory(dir, ec)) {
            return dir;
        }
    }
#else
    if (const char* xdg = std::getenv("XDG_DOWNLOAD_DIR"); xdg && *xdg && std::filesystem::is_directory(xdg, ec)) {
        return xdg;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        const std::filesystem::path dir = std::filesystem::path(home) / "Downloads";
        return std::filesystem::is_directory(dir, ec) ? dir : std::filesystem::path(home);
    }
#endif
    return std::filesystem::current_path(ec);
}

std::expected<std::filesystem::path, std::string> copy_into(const std::filesystem::path& from,
                                                            const std::filesystem::path& dir, std::string_view name) {
    const std::string safe = safe_name(name);
    const auto dot = safe.rfind('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? safe : safe.substr(0, dot);
    const std::string ext = stem.size() == safe.size() ? std::string() : safe.substr(dot);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (int n = 1; n < 1000; ++n) {
        const auto to = dir / to_path(n == 1 ? safe : std::format("{} ({}){}", stem, n, ext));
        if (std::filesystem::exists(to, ec)) {
            continue;
        }
        // skip_existing: another program may have made it in the meantime; then the next name is tried.
        if (std::filesystem::copy_file(from, to, std::filesystem::copy_options::skip_existing, ec)) {
            return to;
        }
        if (ec) {
            return std::unexpected(ec.message());
        }
    }
    return std::unexpected("Too many files with that name already.");
}

} // namespace zchat::file
