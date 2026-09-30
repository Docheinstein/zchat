#pragma once

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string_view>

namespace zchat::sound {

// The sounds of trills (/trill), which everybody else in the chat plays once: MP3 and WAV files, which every system's
// player plays. They travel like files (see file::Trill), and are kept in the temporary folder, by name.

// The biggest sound of a trill, in bytes: about two minutes of MP3 at 128 kbit/s.
inline constexpr std::uintmax_t max_bytes = 2 * 1024 * 1024;

// Whether data looks like an MP3 file (an ID3 tag first, or an MPEG audio frame), a WAV file, or either.
bool is_mp3(std::string_view data);
bool is_wav(std::string_view data);
bool is_audio(std::string_view data);

// Plays a sound file once, at full volume, and returns when it is over, or as soon as stop is requested. Returns
// false when it could not be played (on Linux, when none of the usual players is installed).
// While it plays, the speakers are at 100% and unmuted; then they are put back as they were (once the last sound
// playing is over, when several play at once).
bool play(const std::filesystem::path& path, std::stop_token stop);

} // namespace zchat::sound
