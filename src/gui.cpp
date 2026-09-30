#include "gui.hpp"

#include "config.hpp"
#include "image.hpp"
#include "ui_html.hpp"

#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

// The web view library: third party code, whose warnings are not ours to fix.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <webview/webview.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace zchat {

namespace {

    // JSON, just what the page and zchat say to each other: strings, and arrays of strings.
    std::string json_string(std::string_view s) {
        std::string out = "\"";
        for (const char c : s) {
            const auto u = static_cast<unsigned char>(c);
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (u < 0x20 || u == 0x7F) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(u));
            } else {
                out += c;
            }
        }
        return out + "\"";
    }

    void append_utf8(std::string& out, char32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    // The strings of a JSON array, as the arguments of a bound function come; anything else in it is skipped.
    std::vector<std::string> json_strings(std::string_view json) {
        std::vector<std::string> out;
        std::size_t i = 0;
        const auto hex4 = [&](std::size_t at) -> std::optional<char32_t> {
            if (at + 4 > json.size()) {
                return std::nullopt;
            }
            char32_t v = 0;
            for (std::size_t k = at; k < at + 4; ++k) {
                const char c = json[k];
                v <<= 4;
                if (c >= '0' && c <= '9') {
                    v |= static_cast<char32_t>(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    v |= static_cast<char32_t>(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    v |= static_cast<char32_t>(c - 'A' + 10);
                } else {
                    return std::nullopt;
                }
            }
            return v;
        };
        while (i < json.size()) {
            if (json[i] != '"') {
                ++i;
                continue;
            }
            std::string s;
            ++i;
            while (i < json.size() && json[i] != '"') {
                if (json[i] != '\\' || i + 1 >= json.size()) {
                    s += json[i++];
                    continue;
                }
                const char e = json[i + 1];
                i += 2;
                switch (e) {
                case 'n':
                    s += '\n';
                    break;
                case 't':
                    s += '\t';
                    break;
                case 'r':
                    s += '\r';
                    break;
                case 'b':
                    s += '\b';
                    break;
                case 'f':
                    s += '\f';
                    break;
                case 'u': {
                    auto cp = hex4(i);
                    if (!cp) {
                        break;
                    }
                    i += 4;
                    // A surrogate pair: a character past U+FFFF.
                    if (*cp >= 0xD800 && *cp <= 0xDBFF && i + 6 <= json.size() && json[i] == '\\' &&
                        json[i + 1] == 'u') {
                        if (const auto low = hex4(i + 2); low && *low >= 0xDC00 && *low <= 0xDFFF) {
                            *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*low - 0xDC00);
                            i += 6;
                        }
                    }
                    append_utf8(s, *cp);
                    break;
                }
                default:
                    s += e;
                    break;
                }
            }
            ++i;
            out.push_back(std::move(s));
        }
        return out;
    }

    std::string base64_decode(std::string_view in) {
        static constexpr std::string_view digits =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        unsigned value = 0;
        int bits = 0;
        for (const char c : in) {
            const auto d = digits.find(c);
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

    // The name of a dropped file, safe to use in the drop folder: no folders, nothing odd.
    std::string safe_file_name(std::string_view name) {
        std::string out;
        for (const char c : name) {
            const auto u = static_cast<unsigned char>(c);
            out += (std::isalnum(u) || c == '.' || c == '-' || c == '_' || u >= 0x80) ? c : '_';
        }
        if (out.empty() || out.front() == '.') {
            out.insert(out.begin(), 'x');
        }
        return out.substr(0, 100);
    }

} // namespace

struct Gui::Impl {
    webview::webview view {false, nullptr};

    std::mutex mutex;
    std::condition_variable typed;
    std::deque<std::string> lines;
    bool closed = false;
    bool interrupted = false;
    // Until the page has loaded, what is printed waits here.
    bool ready = false;
    std::vector<std::string> waiting;
    std::string info = "{}";

    std::function<std::vector<Mention>()> mention_source;
    std::function<Mention()> self_source;
    std::function<std::optional<std::string>(std::string_view)> avatar_source;
    std::function<std::optional<std::string>(std::string_view)> rewriter;

    // Runs JavaScript in the page, from any thread.
    void eval(std::string js) {
        view.dispatch([this, js = std::move(js)] {
            view.eval(js);
        });
    }
};

bool Gui::available() {
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    const char* x11 = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    return (x11 && *x11) || (wayland && *wayland);
#endif
}

Gui::Gui() {
#ifdef _WIN32
    // The web view keeps its data (the page's settings) in the config folder, not next to the executable.
    // Unless one is given already (e.g. for a second window with its own).
    if (const auto dir = config::dir();
        !dir.empty() && GetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", nullptr, 0) == 0) {
        std::error_code ec;
        std::filesystem::create_directories(dir / "webview", ec);
        _wputenv_s(L"WEBVIEW2_USER_DATA_FOLDER", (dir / "webview").wstring().c_str());
    }
#endif
    impl_ = std::make_unique<Impl>();
    auto& view = impl_->view;
    view.set_title("zchat");
    view.set_size(1000, 700, WEBVIEW_HINT_NONE);
    view.set_size(560, 360, WEBVIEW_HINT_MIN);

    // The bound functions run on the window's thread, and must not wait for the chat.
    view.bind("zchatReady", [this](const std::string&) -> std::string {
        std::scoped_lock lock(impl_->mutex);
        impl_->ready = true;
        impl_->view.eval(std::format("zchat.info({})", impl_->info));
        for (const auto& line : impl_->waiting) {
            impl_->view.eval(std::format("zchat.print({})", json_string(line)));
        }
        impl_->waiting.clear();
        return "null";
    });
    view.bind("zchatSend", [this](const std::string& args) -> std::string {
        const auto strings = json_strings(args);
        {
            std::scoped_lock lock(impl_->mutex);
            impl_->lines.push_back(strings.empty() ? std::string() : strings.front());
        }
        impl_->typed.notify_all();
        return "null";
    });
    view.bind("zchatMentions", [this](const std::string&) -> std::string {
        std::vector<Mention> people;
        {
            std::scoped_lock lock(impl_->mutex);
            if (impl_->mention_source) {
                people = impl_->mention_source();
            }
        }
        std::string out = "[";
        for (const auto& m : people) {
            out += out.size() > 1 ? "," : "";
            out += "[" + json_string(m.name) + "," + json_string(m.style) + "," + json_string(m.avatar) + "]";
        }
        return out + "]";
    });
    view.bind("zchatSelf", [this](const std::string&) -> std::string {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->self_source) {
            return "null";
        }
        const Mention self = impl_->self_source();
        return "[" + json_string(self.name) + "," + json_string(self.style) + "," + json_string(self.avatar) + "]";
    });
    // An avatar, by hash: [[src, delay], ...] as for zchat.image(), or null when it is not here (yet).
    view.bind("zchatAvatar", [this](const std::string& args) -> std::string {
        const auto strings = json_strings(args);
        std::function<std::optional<std::string>(std::string_view)> source;
        {
            std::scoped_lock lock(impl_->mutex);
            source = impl_->avatar_source;
        }
        if (strings.empty() || !source) {
            return "null";
        }
        const auto text = source(strings.front());
        const auto picture = text ? image::parse_picture(*text) : std::nullopt;
        if (!picture) {
            return "null";
        }
        std::string list = "[";
        for (const auto& frame : picture->frames) {
            list += list.size() > 1 ? "," : "";
            list += std::format("[\"data:{};base64,{}\",{}]", frame.mime, frame.base64, frame.delay_ms);
        }
        return list + "]";
    });
    view.bind("zchatRewrite", [this](const std::string& args) -> std::string {
        const auto strings = json_strings(args);
        std::scoped_lock lock(impl_->mutex);
        if (strings.empty() || !impl_->rewriter) {
            return "null";
        }
        const auto rewritten = impl_->rewriter(strings.front());
        return rewritten ? json_string(*rewritten) : "null";
    });
    // A dropped file: the page cannot give its path, only its contents, which are saved in the temporary folder.
    view.bind("zchatDrop", [](const std::string& args) -> std::string {
        const auto strings = json_strings(args);
        if (strings.size() < 2) {
            return "null";
        }
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path(ec) / "zchat-drops";
        std::filesystem::create_directories(dir, ec);
        // (safe_file_name() turns folder separators into '_', so it stays in the drop folder.)
        const std::string name = safe_file_name(strings[0]);
        const auto file = dir / std::filesystem::path(std::u8string(name.begin(), name.end()));
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        const std::string data = base64_decode(strings[1]);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out.flush()) {
            return "null";
        }
        const auto u8 = file.u8string();
        return json_string(std::string(u8.begin(), u8.end()));
    });
    // The text size and font of the window, in the config file with the other settings.
    view.bind("zchatSettings", [](const std::string&) -> std::string {
        return "[" + json_string(config::get("window_text_size").value_or("")) + "," +
               json_string(config::get("window_font").value_or("")) + "]";
    });
    view.bind("zchatSaveSettings", [](const std::string& args) -> std::string {
        const auto strings = json_strings(args);
        if (strings.size() >= 2) {
            config::set("window_text_size", strings[0]);
            config::set("window_font", strings[1]);
        }
        return "null";
    });
    view.set_html(std::string(reinterpret_cast<const char*>(ui_html), ui_html_size));
}

Gui::~Gui() = default;

void Gui::set_info(std::uint16_t port) {
    std::scoped_lock lock(impl_->mutex);
    impl_->info = std::format("{{\"port\":{}}}", port);
    if (impl_->ready) {
        impl_->eval(std::format("zchat.info({})", impl_->info));
    }
}

void Gui::run() {
    impl_->view.run();
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->closed = true;
    }
    impl_->typed.notify_all();
}

void Gui::close() {
    impl_->view.dispatch([this] {
        impl_->view.terminate();
    });
}

void Gui::set_prompt(std::string, std::size_t) {
    // The window has its own input box.
}

void Gui::print(std::string_view line) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->ready) {
        impl_->waiting.emplace_back(line);
        return;
    }
    impl_->eval(std::format("zchat.print({})", json_string(line)));
}

std::optional<std::string> Gui::read_line() {
    std::unique_lock lock(impl_->mutex);
    impl_->typed.wait(lock, [&] {
        return !impl_->lines.empty() || impl_->closed || impl_->interrupted;
    });
    if (impl_->closed || impl_->interrupted) {
        return std::nullopt;
    }
    std::string line = std::move(impl_->lines.front());
    impl_->lines.pop_front();
    return line;
}

void Gui::set_mentions(std::function<std::vector<Mention>()> source) {
    std::scoped_lock lock(impl_->mutex);
    impl_->mention_source = std::move(source);
}

void Gui::set_self(std::function<Mention()> source) {
    std::scoped_lock lock(impl_->mutex);
    impl_->self_source = std::move(source);
}

void Gui::set_avatars(std::function<std::optional<std::string>(std::string_view hash)> source) {
    std::scoped_lock lock(impl_->mutex);
    impl_->avatar_source = std::move(source);
}

void Gui::set_rewriter(std::function<std::optional<std::string>(std::string_view)> rewriter) {
    std::scoped_lock lock(impl_->mutex);
    impl_->rewriter = std::move(rewriter);
}

void Gui::bell() {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->ready) {
        impl_->eval("zchat.bell()");
    }
}

bool Gui::show_image(std::string_view line, int width, int height, const std::vector<Frame>& frames) {
    // [[data: URL, delay], ...]; base64 needs no escaping in a JSON string.
    std::string list = "[";
    for (const auto& frame : frames) {
        list += list.size() > 1 ? "," : "";
        list += std::format("[\"data:{};base64,{}\",{}]", frame.mime, frame.base64, frame.delay_ms);
    }
    list += "]";
    const std::string js = std::format("zchat.image({},{},{},{})", json_string(line), width, height, list);
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->ready) {
        // Before the page is loaded: nothing can be shown yet, the chat draws it instead (only at start, if ever).
        return false;
    }
    impl_->eval(js);
    return true;
}

bool Gui::show_file(std::string_view line, std::size_t index, std::string_view name, std::string_view size) {
    const std::string js =
        std::format("zchat.file({},{},{},{})", json_string(line), index, json_string(name), json_string(size));
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->ready) {
        return false;
    }
    impl_->eval(js);
    return true;
}

void Gui::interrupt() {
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->interrupted = true;
    }
    impl_->typed.notify_all();
}

} // namespace zchat
