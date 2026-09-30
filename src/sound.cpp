#include "sound.hpp"

#include <mutex>
#include <optional>
#include <string>

#ifdef _WIN32
#include <atomic>
#include <chrono>
#include <format>
#include <thread>
#include <windows.h>

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#else
#include "process.hpp"

#include <cctype>
#include <chrono>
#include <format>
#include <vector>
#endif

namespace zchat::sound {

namespace {

#ifdef _WIN32
    // The volume of the speakers: the output device, its level from 0 to 1, and whether it is muted.
    struct Volume {
        std::wstring device;
        float level = 0;
        BOOL muted = FALSE;
    };

    // COM for the time of a call (Core Audio needs it): MCI, on the same thread, sets it up its own way.
    class Com {
    public:
        Com() :
            result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
        }
        ~Com() {
            if (SUCCEEDED(result_)) {
                CoUninitialize();
            }
        }
        Com(const Com&) = delete;
        Com& operator=(const Com&) = delete;

    private:
        HRESULT result_;
    };

    // The volume control of an output device: the one named, or else the default one (whose name goes in name).
    IAudioEndpointVolume* open_volume(const std::wstring& device, std::wstring* name) {
        IMMDeviceEnumerator* devices = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&devices)))) {
            return nullptr;
        }
        IMMDevice* endpoint = nullptr;
        const HRESULT found = device.empty() ? devices->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint)
                                             : devices->GetDevice(device.c_str(), &endpoint);
        devices->Release();
        if (FAILED(found)) {
            return nullptr;
        }
        if (LPWSTR id = nullptr; name && SUCCEEDED(endpoint->GetId(&id))) {
            *name = id;
            CoTaskMemFree(id);
        }
        IAudioEndpointVolume* volume = nullptr;
        if (FAILED(endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(&volume)))) {
            volume = nullptr;
        }
        endpoint->Release();
        return volume;
    }

    std::optional<Volume> get_volume() {
        const Com com;
        Volume v;
        IAudioEndpointVolume* volume = open_volume({}, &v.device);
        if (!volume) {
            return std::nullopt;
        }
        const bool read =
            SUCCEEDED(volume->GetMasterVolumeLevelScalar(&v.level)) && SUCCEEDED(volume->GetMute(&v.muted));
        volume->Release();
        return read ? std::optional(v) : std::nullopt;
    }

    void set_volume(const Volume& v) {
        const Com com;
        if (IAudioEndpointVolume* volume = open_volume(v.device, nullptr)) {
            volume->SetMasterVolumeLevelScalar(v.level, nullptr);
            volume->SetMute(v.muted, nullptr);
            volume->Release();
        }
    }

    Volume loudest(const Volume& v) {
        return {v.device, 1.0f, FALSE};
    }
#else
    // What a quick command prints, or nullopt when it fails (or is not installed). In English, to be understood.
    std::optional<std::string> ask(std::vector<std::string> args) {
        args.insert(args.begin(), {"env", "LC_ALL=C"});
        auto result = process::run(args, {}, std::chrono::seconds(3));
        return result.exit_code == 0 ? std::optional(std::move(result.output)) : std::nullopt;
    }

    std::string trim(std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
            s.pop_back();
        }
        return s;
    }

#ifdef __APPLE__
    // The output volume, from 0 to 100, and whether it is muted.
    struct Volume {
        int level = 0;
        bool muted = false;
    };

    std::optional<Volume> get_volume() {
        // "35 false"; "missing value" for a device whose volume cannot be changed.
        const auto out = ask({"osascript", "-e", "set s to get volume settings", "-e",
                              "return (output volume of s as text) & \" \" & (output muted of s as text)"});
        if (!out) {
            return std::nullopt;
        }
        Volume v;
        const auto space = out->find(' ');
        const std::string level = out->substr(0, space);
        if (space == std::string::npos || level.empty() || level.size() > 3 ||
            level.find_first_not_of("0123456789") != std::string::npos) {
            return std::nullopt;
        }
        v.level = std::stoi(level);
        v.muted = trim(out->substr(space + 1)) == "true";
        return v;
    }

    void set_volume(const Volume& v) {
        ask({"osascript", "-e",
             std::format("set volume output volume {} {} output muted", v.level, v.muted ? "with" : "without")});
    }

    Volume loudest(const Volume&) {
        return {100, false};
    }
#else
    // PulseAudio (or PipeWire): the default output, the level of each of its channels ("50%"), and whether it is
    // muted.
    struct Volume {
        std::string sink;
        std::vector<std::string> levels;
        bool muted = false;
    };

    std::optional<Volume> get_volume() {
        Volume v;
        const auto sink = ask({"pactl", "get-default-sink"});
        v.sink = sink ? trim(*sink) : std::string();
        // "Volume: front-left: 32768 /  50% / -18.06 dB,   front-right: 32768 /  50% / -18.06 dB", and "Mute: no".
        const auto volume = v.sink.empty() ? std::nullopt : ask({"pactl", "get-sink-volume", v.sink});
        const auto mute = v.sink.empty() ? std::nullopt : ask({"pactl", "get-sink-mute", v.sink});
        if (!volume || !mute) {
            return std::nullopt;
        }
        for (auto percent = volume->find('%'); percent != std::string::npos; percent = volume->find('%', percent + 1)) {
            auto start = percent;
            while (start > 0 && std::isdigit(static_cast<unsigned char>((*volume)[start - 1]))) {
                --start;
            }
            if (start < percent) {
                v.levels.push_back(volume->substr(start, percent - start + 1));
            }
        }
        v.muted = mute->find("yes") != std::string::npos;
        return v.levels.empty() ? std::nullopt : std::optional(v);
    }

    void set_volume(const Volume& v) {
        std::vector<std::string> args {"pactl", "set-sink-volume", v.sink};
        args.insert(args.end(), v.levels.begin(), v.levels.end());
        ask(args);
        ask({"pactl", "set-sink-mute", v.sink, v.muted ? "1" : "0"});
    }

    Volume loudest(const Volume& v) {
        return {v.sink, {"100%"}, false};
    }
#endif
#endif

    // Full volume while it lives: the first of the sounds playing at once saves the volume, the last puts it back.
    class Loud {
    public:
        Loud() {
            std::scoped_lock lock(mutex_);
            if (playing_++ == 0 && (saved_ = get_volume())) {
                set_volume(loudest(*saved_));
            }
        }
        ~Loud() {
            std::scoped_lock lock(mutex_);
            if (--playing_ == 0 && saved_) {
                set_volume(*saved_);
                saved_.reset();
            }
        }
        Loud(const Loud&) = delete;
        Loud& operator=(const Loud&) = delete;

    private:
        static inline std::mutex mutex_;
        static inline int playing_ = 0;
        static inline std::optional<Volume> saved_;
    };

} // namespace

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
    bool played = false;
    {
        // Loud only once it is ready to start, and until it is over.
        const Loud loud;
        played = command(L"play " + alias);
        // It plays on threads of its own: asked now and then whether it is over, so a stop does not wait for the end.
        wchar_t mode[32] = {};
        while (played && !stop.stop_requested() && command(L"status " + alias + L" mode", mode, 32) &&
               std::wstring_view(mode) != L"stopped") {
            std::this_thread::sleep_for(100ms);
        }
        command(L"close " + alias);
    }
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
    const Loud loud;
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
