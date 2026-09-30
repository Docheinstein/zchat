#include "sound.hpp"

#include <string>

#ifdef _WIN32
#include <atomic>
#include <chrono>
#include <format>
#include <thread>
#include <windows.h>

#include <mmsystem.h>
#else
#include "process.hpp"

#include <vector>
#endif

namespace zchat::sound {

bool is_mp3(std::string_view data) {
    if (data.starts_with("ID3")) {
        return true;
    }
    // An MPEG audio frame starts with 11 bits set.
    return data.size() >= 2 && static_cast<unsigned char>(data[0]) == 0xFF &&
           (static_cast<unsigned char>(data[1]) & 0xE0) == 0xE0;
}

#ifdef _WIN32
bool play(const std::filesystem::path& path, std::stop_token stop) {
    using namespace std::chrono_literals;
    // MCI plays MP3 files with the system's own decoder; each sound under a name of its own, so several can play at
    // once.
    static std::atomic<unsigned> next {0};
    const std::wstring alias = std::format(L"zchat_trill_{}", next++);
    const auto command = [](const std::wstring& text, wchar_t* answer = nullptr, UINT size = 0) {
        return mciSendStringW(text.c_str(), answer, size, nullptr) == 0;
    };
    if (!command(std::format(L"open \"{}\" type mpegvideo alias {}", path.wstring(), alias))) {
        return false;
    }
    const bool played = command(L"play " + alias);
    // It plays on threads of its own: asked now and then whether it is over, so a stop does not wait for the end.
    wchar_t mode[32] = {};
    while (played && !stop.stop_requested() && command(L"status " + alias + L" mode", mode, 32) &&
           std::wstring_view(mode) != L"stopped") {
        std::this_thread::sleep_for(100ms);
    }
    command(L"close " + alias);
    return played;
}
#else
bool play(const std::filesystem::path& path, std::stop_token stop) {
    const auto u8 = path.u8string();
    const std::string file(u8.begin(), u8.end());
    // The first player there is that plays it.
    const std::vector<std::vector<std::string>> players {
#ifdef __APPLE__
        {"afplay", file},
#endif
        {"mpg123", "-q", file},
        {"ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", file},
        {"mpv", "--no-video", "--really-quiet", file},
        {"paplay", file},
    };
    for (const auto& player : players) {
        if (process::run(player, stop).exit_code == 0) {
            return true;
        }
        if (stop.stop_requested()) {
            break;
        }
    }
    return false;
}
#endif

} // namespace zchat::sound
