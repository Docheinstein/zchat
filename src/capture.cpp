// Screen capture for /spy (see capture.hpp). There is no portable way to grab the screen in C++, so this runs the
// screenshot tool that comes with the system and reads the files it writes:
//   macOS    screencapture, which writes one file per display when given several file names.
//   Windows  a short PowerShell script that saves each Screen.AllScreens to its own PNG.
//   Linux    on X11, ImageMagick's import per monitor (their geometry from xrandr), or the whole root window; on
//            Wayland, grim; falling back to scrot, maim, gnome-screenshot or spectacle for the whole desktop.
// Everything is best-effort and guarded: capture_screens() never throws and never blocks the chat for long (the
// tools are run with a timeout), and it is only ever called once the person agreed to be spied on (see spy.cpp).

#include "capture.hpp"

#include "process.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <format>
#include <optional>
#include <random>
#include <string_view>
#include <system_error>

namespace zchat::capture {

namespace {

    using namespace std::chrono_literals;
    // Long enough for a slow machine with several monitors, short enough never to hang the spy.
    constexpr auto capture_timeout = 20s;
    // No machine we send pictures of has more screens than this; extra candidate files are simply never written.
    constexpr int max_monitors = 8;

    // A fresh temporary folder for one capture, so its files do not clash with another's. Empty on failure.
    std::filesystem::path make_temp_dir() {
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        if (ec) {
            return {};
        }
        static std::mt19937_64 rng(std::random_device {}());
        for (int tries = 0; tries < 8; ++tries) {
            const auto dir = base / "zchat-spy" / std::format("{:016x}", rng());
            if (std::filesystem::create_directories(dir, ec) && !ec) {
                return dir;
            }
        }
        return {};
    }

    // The files in dir that exist and are not empty, sorted by name (screen0, screen1, ...), so empty candidate
    // files (monitors that were not there) are dropped.
    std::vector<std::filesystem::path> written_files(const std::filesystem::path& dir) {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && entry.file_size(ec) > 0) {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);
        return files;
    }

    bool have_tool(std::string_view tool, std::stop_token stop) {
#ifdef _WIN32
        (void)tool;
        (void)stop;
        return true;
#else
        // `which TOOL` says whether it is in the PATH, quietly.
        return process::run({"which", std::string(tool)}, stop, 3s).exit_code == 0;
#endif
    }

#ifndef _WIN32
    std::optional<long long> to_int(std::string_view s) {
        long long value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (ptr != s.data() + s.size() || ec != std::errc {}) {
            return std::nullopt;
        }
        return value;
    }
#endif

#ifdef __APPLE__
    // macOS: screencapture writes each display to one of the file names it is given.
    std::expected<std::vector<std::filesystem::path>, std::string> capture_macos(const std::filesystem::path& dir,
                                                                                 std::stop_token stop) {
        std::vector<std::string> args {"screencapture", "-x", "-t", "png"};
        for (int i = 0; i < max_monitors; ++i) {
            args.push_back((dir / std::format("screen{}.png", i)).string());
        }
        const auto result = process::run(args, stop, capture_timeout);
        if (result.timed_out) {
            return std::unexpected("the screenshot took too long");
        }
        auto files = written_files(dir);
        if (files.empty()) {
            return std::unexpected("screencapture took no picture (screen recording permission may be off)");
        }
        return files;
    }
#elif !defined(_WIN32)
    // Linux/BSD: ImageMagick's import can grab the whole X11 root window, and crop each monitor out of it when
    // xrandr lists their geometry; otherwise one of the lot of tools below grabs the whole desktop.

    // The monitors' geometry from `xrandr --listmonitors`, each "WxH+X+Y"; empty when it cannot be worked out.
    std::vector<std::string> x11_monitors(std::stop_token stop) {
        std::vector<std::string> geometries;
        if (!have_tool("xrandr", stop)) {
            return geometries;
        }
        const auto result = process::run({"xrandr", "--listmonitors"}, stop, 5s);
        if (result.exit_code != 0) {
            return geometries;
        }
        // Lines like " 0: +*eDP-1 1920/344x1080/194+0+0  eDP-1"; the field with '+' is "WIDTH/mmWxHEIGHT/mmH+X+Y".
        std::string_view text = result.output;
        while (!text.empty()) {
            const auto nl = text.find('\n');
            std::string_view line = text.substr(0, nl);
            text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
            const auto plus = line.find('+');
            if (plus == std::string_view::npos || line.find('x') == std::string_view::npos) {
                continue;
            }
            // Pick the whitespace-separated token that holds the geometry (the one with '/').
            std::string_view token;
            std::size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && line[i] == ' ') {
                    ++i;
                }
                const std::size_t start = i;
                while (i < line.size() && line[i] != ' ') {
                    ++i;
                }
                const std::string_view word = line.substr(start, i - start);
                if (word.find('/') != std::string_view::npos && word.find('+') != std::string_view::npos) {
                    token = word;
                }
            }
            if (token.empty()) {
                continue;
            }
            // "1920/344x1080/194+0+0" -> "1920x1080+0+0".
            long long w = 0, h = 0, x = 0, y = 0;
            const auto xpos = token.find('x');
            const auto plus1 = token.find('+');
            const auto plus2 = token.find('+', plus1 + 1);
            const auto slash1 = token.find('/');
            const auto slash2 = token.find('/', xpos);
            if (xpos == std::string_view::npos || plus1 == std::string_view::npos ||
                plus2 == std::string_view::npos || slash1 == std::string_view::npos ||
                slash2 == std::string_view::npos) {
                continue;
            }
            const auto wv = to_int(token.substr(0, slash1));
            const auto hv = to_int(token.substr(xpos + 1, slash2 - xpos - 1));
            const auto xv = to_int(token.substr(plus1 + 1, plus2 - plus1 - 1));
            const auto yv = to_int(token.substr(plus2 + 1));
            if (!wv || !hv || !xv || !yv) {
                continue;
            }
            w = *wv;
            h = *hv;
            x = *xv;
            y = *yv;
            if (w > 0 && h > 0) {
                geometries.push_back(std::format("{}x{}+{}+{}", w, h, x, y));
            }
        }
        return geometries;
    }

    std::expected<std::vector<std::filesystem::path>, std::string> capture_linux(const std::filesystem::path& dir,
                                                                                 std::stop_token stop) {
        const bool wayland = std::getenv("WAYLAND_DISPLAY") != nullptr;
        const bool x11 = std::getenv("DISPLAY") != nullptr;
        if (!wayland && !x11) {
            return std::unexpected("no graphical display on this machine");
        }

        // X11 with ImageMagick: one picture per monitor, cropped out of the root window.
        if (x11 && have_tool("import", stop)) {
            const auto monitors = x11_monitors(stop);
            if (monitors.size() > 1) {
                for (std::size_t i = 0; i < monitors.size() && i < max_monitors; ++i) {
                    const auto file = dir / std::format("screen{}.png", i);
                    process::run({"import", "-silent", "-window", "root", "-crop", monitors[i], "+repage",
                                  file.string()},
                                 stop, capture_timeout);
                }
                if (auto files = written_files(dir); !files.empty()) {
                    return files;
                }
            }
        }

        // Otherwise, the whole desktop (every monitor together) with whichever tool is installed.
        const auto whole = dir / "screen0.png";
        const std::string out = whole.string();
        struct Tool {
            std::string_view name;
            std::vector<std::string> args;
            bool only_wayland = false;
            bool only_x11 = false;
        };
        const std::array tools {
            Tool {"grim", {"grim", out}, true, false},
            Tool {"import", {"import", "-silent", "-window", "root", out}, false, true},
            Tool {"maim", {"maim", out}, false, true},
            Tool {"scrot", {"scrot", "-o", out}, false, true},
            Tool {"spectacle", {"spectacle", "-b", "-n", "-f", "-o", out}, false, false},
            Tool {"gnome-screenshot", {"gnome-screenshot", "-f", out}, false, false},
        };
        for (const Tool& tool : tools) {
            if ((tool.only_wayland && !wayland) || (tool.only_x11 && !x11) || !have_tool(tool.name, stop)) {
                continue;
            }
            process::run(tool.args, stop, capture_timeout);
            if (auto files = written_files(dir); !files.empty()) {
                return files;
            }
        }
        return std::unexpected("no screenshot tool found (install grim, scrot, maim, imagemagick or gnome-screenshot)");
    }
#endif

#ifdef _WIN32
    // Windows: a small PowerShell script saves each monitor (System.Windows.Forms.Screen.AllScreens) to its own PNG.
    std::expected<std::vector<std::filesystem::path>, std::string> capture_windows(const std::filesystem::path& dir,
                                                                                   std::stop_token stop) {
        const std::string out = dir.string();
        const std::string script =
            "Add-Type -AssemblyName System.Windows.Forms,System.Drawing;"
            "$i=0;"
            "foreach($s in [System.Windows.Forms.Screen]::AllScreens){"
            "$b=$s.Bounds;"
            "$bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height;"
            "$g=[System.Drawing.Graphics]::FromImage($bmp);"
            "$g.CopyFromScreen($b.X,$b.Y,0,0,$bmp.Size);"
            "$p=Join-Path '" +
            out +
            "' ('screen{0}.png' -f $i);"
            "$bmp.Save($p,[System.Drawing.Imaging.ImageFormat]::Png);"
            "$g.Dispose();$bmp.Dispose();$i++;}";
        const auto result = process::run(
            {"powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script}, stop,
            capture_timeout);
        if (result.timed_out) {
            return std::unexpected("the screenshot took too long");
        }
        auto files = written_files(dir);
        if (files.empty()) {
            return std::unexpected("could not capture the screen");
        }
        return files;
    }
#endif

} // namespace

std::expected<std::vector<std::filesystem::path>, std::string> capture_screens(std::stop_token stop) {
    const auto dir = make_temp_dir();
    if (dir.empty()) {
        return std::unexpected("no place to keep the screenshot");
    }
#if defined(_WIN32)
    return capture_windows(dir, stop);
#elif defined(__APPLE__)
    return capture_macos(dir, stop);
#else
    return capture_linux(dir, stop);
#endif
}

} // namespace zchat::capture
