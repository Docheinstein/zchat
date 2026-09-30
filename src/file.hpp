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
//
// Trills (see sound.hpp) are sent the same way, as "trill SIZE NAME": MP3 files that everybody plays once.
enum class Kind { File, Trill };

// The biggest file (or trill) that can be sent, in bytes: in base64, with its first line, a file fits in an Image's
// text.
std::uintmax_t max_bytes(Kind kind = Kind::File);

// Whether a line of input is nothing but the full path of an existing file, as when one is dropped on the terminal or
// the window (see image::parse_path()).
bool is_dropped_file(std::string_view line);

// The text of a file (or trill) to send, or why it cannot be sent.
std::expected<std::string, std::string> encode(const std::filesystem::path& path, Kind kind = Kind::File);
// The same from its name and bytes, which must fit (see max_bytes()).
std::string encode(std::string_view name, std::string_view data, Kind kind = Kind::File);

// A received file: its name (safe to use as a file name, see safe_name()) and its bytes.
struct Received {
    std::string name;
    std::string data;
};
// Returns nullopt for anything that is not the text of a file (or of a trill, an MP3 file).
std::optional<Received> parse(std::string_view text, Kind kind = Kind::File);

// Whether the text of an Image is a file (or a trill) rather than a picture.
bool is_file(std::string_view text);
bool is_trill(std::string_view text);

// A name safe to use as a file name in a folder of ours: no folders, nothing odd, nothing Windows keeps for itself.
std::string safe_name(std::string_view name);

// A size for people to read: "512 bytes", "1.4 MB".
std::string format_size(std::uintmax_t bytes);

// The folder files are saved in: the user's downloads folder, or else the home folder.
std::filesystem::path downloads_dir();

// Copies a file into a folder under a name, with " (2)", " (3)"... before its extension when that name is taken.
// Returns where it went, or why it could not.
std::expected<std::filesystem::path, std::string> copy_into(const std::filesystem::path& from,
                                                            const std::filesystem::path& dir, std::string_view name);

} // namespace zchat::file
