#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace zchat::file {

// Files of any kind, sent like pictures (see PacketType::Image): "file SIZE NAME", '\n' and the file in base64
// (NAME in base64 too).
// Whoever gets one keeps it in a temporary folder, and can save it in their downloads folder.

// The biggest file that can be sent, in bytes: in base64, with its first line, it fits in an Image's text.
std::uintmax_t max_bytes();

// Whether a line of input is nothing but the full path of an existing file, as when one is dropped on the terminal or
// the window (see image::parse_path()).
bool is_dropped_file(std::string_view line);

// The bytes of a file of at most max_bytes, or why it cannot be read.
std::expected<std::string, std::string> read(const std::filesystem::path& path, std::uintmax_t max_bytes);

// The text of a file to send, or why it cannot be sent.
std::expected<std::string, std::string> encode(const std::filesystem::path& path);

// A received file: its name (safe to use as a file name, see safe_name()) and its bytes.
struct Received {
    std::string name;
    std::string data;
};
// Returns nullopt for anything that is not the text of a file.
std::optional<Received> parse(std::string_view text);

// Whether the text of an Image is a file (or a trill) rather than a picture.
bool is_file(std::string_view text);
bool is_trill(std::string_view text);

// Trills (/trill): a sound and a picture, or just one of them, that everybody else gets once, sent the same way:
// "trill SOUND_SIZE SOUND_NAME PICTURE_NAME" (the names in base64; "0 -" and "-" for none), '\n', the sound in
// base64, '\n' and the picture, as image::encode_picture() makes it. (The trills of before pictures, "trill SIZE NAME"
// and the sound, are still understood.)
struct Trill {
    // The sound (see sound::is_audio()) and its name; empty for none.
    std::string sound_name;
    std::string sound;
    // The picture (see image::parse_picture()) and the name of its file; empty for none.
    std::string picture_name;
    std::string picture;
};
// The biggest picture of a trill, in bytes of its text.
inline constexpr std::size_t max_trill_picture_bytes = 4'000'000;
std::string encode_trill(const Trill& trill);
// Returns nullopt for anything that is not a trill with a sound or a picture (or both) that can be played and shown.
std::optional<Trill> parse_trill(std::string_view text);

// A name safe to use as a file name in a folder of ours: no folders, nothing odd, nothing Windows keeps for itself.
// At most max_bytes long.
inline constexpr std::size_t max_name_bytes = 200;
std::string safe_name(std::string_view name, std::size_t max_bytes = max_name_bytes);

// A size for people to read: "512 bytes", "1.4 MB".
std::string format_size(std::uintmax_t bytes);

// The folder files are saved in: the user's downloads folder, or else the home folder.
std::filesystem::path downloads_dir();

// Copies a file into a folder under a name, with " (2)", " (3)"... before its extension when that name is taken.
// Returns where it went, or why it could not.
std::expected<std::filesystem::path, std::string> copy_into(const std::filesystem::path& from,
                                                            const std::filesystem::path& dir, std::string_view name);

} // namespace zchat::file
