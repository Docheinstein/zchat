#pragma once

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string_view>

namespace zchat::sound {

// Trills (/trill): MP3 files that everybody in the chat plays once, as they arrive. They travel like files (see
// file::Kind::Trill), and are kept in the temporary folder only while they play.

// The biggest trill, in bytes: about two minutes of sound at 128 kbit/s.
inline constexpr std::uintmax_t max_bytes = 2 * 1024 * 1024;

// Whether data looks like an MP3 file: an ID3 tag first, or an MPEG audio frame.
bool is_mp3(std::string_view data);

// Plays a sound file once, and returns when it is over, or as soon as stop is requested. Returns false when it
// could not be played (on Linux, when none of the usual players is installed).
bool play(const std::filesystem::path& path, std::stop_token stop);

} // namespace zchat::sound
