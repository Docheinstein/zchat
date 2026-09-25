#include "terminal.hpp"

#include "protocol.hpp"
#include "text.hpp"

#include <cstdio>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace zchat {

namespace {

    constexpr char32_t key_ctrl_c = 0x03;
    constexpr char32_t key_ctrl_d = 0x04;
    constexpr char32_t key_backspace = 0x08;
    constexpr char32_t key_ctrl_u = 0x15;
    constexpr char32_t key_ctrl_w = 0x17;
    constexpr char32_t key_ctrl_z = 0x1A;
    constexpr char32_t key_delete = 0x7F;

} // namespace

#ifdef _WIN32
struct Terminal::Platform {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD in_mode = 0;
    DWORD out_mode = 0;
    UINT in_cp = GetConsoleCP();
    UINT out_cp = GetConsoleOutputCP();
    bool console_in = false;
    bool console_out = false;
    char16_t high_surrogate = 0;
};

Terminal::Terminal() :
    platform_(std::make_unique<Platform>()) {
    auto& p = *platform_;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    p.console_in = GetConsoleMode(p.in, &p.in_mode) != 0;
    p.console_out = GetConsoleMode(p.out, &p.out_mode) != 0;
    if (p.console_out) {
        vt_ = SetConsoleMode(p.out, p.out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }
    interactive_ = p.console_in && p.console_out;
    if (interactive_) {
        // Raw keys: no line editing or echo by the console, and Ctrl+C delivered as a key instead of a signal.
        DWORD mode = p.in_mode &
                     ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
        SetConsoleMode(p.in, mode);
    }
}

Terminal::~Terminal() {
    auto& p = *platform_;
    if (interactive_) {
        std::scoped_lock lock(mutex_);
        write(clear_line_locked());
    }
    if (p.console_in) {
        SetConsoleMode(p.in, p.in_mode);
    }
    if (p.console_out) {
        SetConsoleMode(p.out, p.out_mode);
    }
    SetConsoleCP(p.in_cp);
    SetConsoleOutputCP(p.out_cp);
}

void Terminal::write(std::string_view data) {
    auto& p = *platform_;
    if (p.console_out) {
        // Writing UTF-16 is the most reliable way to get Unicode on every Windows console.
        const int n = MultiByteToWideChar(CP_UTF8, 0, data.data(), static_cast<int>(data.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, data.data(), static_cast<int>(data.size()), wide.data(), n);
        DWORD written = 0;
        WriteConsoleW(p.out, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
    } else {
        std::fwrite(data.data(), 1, data.size(), stdout);
        std::fflush(stdout);
    }
}

std::size_t Terminal::width() const {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(platform_->out, &info)) {
        return static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);
    }
    return 80;
}

std::optional<std::string> Terminal::read_line_interactive() {
    auto& p = *platform_;
    while (true) {
        INPUT_RECORD record;
        DWORD count = 0;
        if (!ReadConsoleInputW(p.in, &record, 1, &count)) {
            return std::nullopt;
        }
        if (count == 0 || record.EventType != KEY_EVENT) {
            if (count == 1 && record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                std::scoped_lock lock(mutex_);
                redraw_locked();
            }
            continue;
        }
        const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
        if (!key.bKeyDown || key.uChar.UnicodeChar == 0) {
            continue;
        }
        const auto unit = static_cast<char16_t>(key.uChar.UnicodeChar);
        char32_t cp = unit;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            p.high_surrogate = unit;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (p.high_surrogate == 0) {
                continue;
            }
            cp = 0x10000 + ((static_cast<char32_t>(p.high_surrogate) - 0xD800) << 10) + (unit - 0xDC00);
            p.high_surrogate = 0;
        }
        for (WORD i = 0; i < (key.wRepeatCount ? key.wRepeatCount : 1); ++i) {
            bool quit = false;
            if (on_char(cp, quit)) {
                std::scoped_lock lock(mutex_);
                std::string line = std::move(buffer_);
                buffer_.clear();
                redraw_locked();
                return line;
            }
            if (quit) {
                return std::nullopt;
            }
        }
    }
}
#else
struct Terminal::Platform {
    termios original {};
};

Terminal::Terminal() :
    platform_(std::make_unique<Platform>()) {
    vt_ = isatty(STDOUT_FILENO) != 0;
    interactive_ = vt_ && isatty(STDIN_FILENO) != 0 && tcgetattr(STDIN_FILENO, &platform_->original) == 0;
    if (interactive_) {
        termios raw = platform_->original;
        // Raw keys: no line editing or echo by the tty, and Ctrl+C delivered as a key instead of SIGINT.
        raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~(IXON | ICRNL);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }
}

Terminal::~Terminal() {
    if (interactive_) {
        {
            std::scoped_lock lock(mutex_);
            write(clear_line_locked());
        }
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &platform_->original);
    }
}

void Terminal::write(std::string_view data) {
    std::fwrite(data.data(), 1, data.size(), stdout);
    std::fflush(stdout);
}

std::size_t Terminal::width() const {
    winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 80;
}

namespace {

    // Reads one byte, waiting at most timeout_ms (or forever if negative). Returns -1 on EOF/error/timeout.
    int read_byte(int timeout_ms = -1) {
        while (true) {
            if (timeout_ms >= 0) {
                pollfd pfd {STDIN_FILENO, POLLIN, 0};
                if (poll(&pfd, 1, timeout_ms) <= 0) {
                    return -1;
                }
            }
            unsigned char c;
            const auto n = ::read(STDIN_FILENO, &c, 1);
            if (n == 1) {
                return c;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return -1;
        }
    }

    // Skips the rest of an escape sequence (arrow keys, function keys, ...), which zchat does not use.
    void skip_escape_sequence() {
        int c = read_byte(30);
        if (c != '[' && c != 'O') {
            return;
        }
        do {
            c = read_byte(30);
        } while (c >= 0 && !(c >= 0x40 && c <= 0x7E));
    }

} // namespace

std::optional<std::string> Terminal::read_line_interactive() {
    while (true) {
        int c = read_byte();
        if (c < 0) {
            return std::nullopt;
        }
        if (c == 0x1B) {
            skip_escape_sequence();
            continue;
        }
        char32_t cp = static_cast<char32_t>(c);
        if (c >= 0x80) {
            // Collect the rest of a UTF-8 sequence.
            std::string bytes(1, static_cast<char>(c));
            const int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
            for (int i = 1; i < len; ++i) {
                const int cc = read_byte(30);
                if (cc < 0) {
                    break;
                }
                bytes += static_cast<char>(cc);
            }
            const std::string clean = text::sanitize(bytes, bytes.size());
            if (clean.size() != bytes.size() || clean.empty()) {
                continue;
            }
            cp = 0;
            for (std::size_t i = 0; i < clean.size(); ++i) {
                const auto b = static_cast<unsigned char>(clean[i]);
                cp = i == 0 ? (b & (0x7F >> clean.size())) : ((cp << 6) | (b & 0x3F));
            }
        }
        bool quit = false;
        if (on_char(cp, quit)) {
            std::scoped_lock lock(mutex_);
            std::string line = std::move(buffer_);
            buffer_.clear();
            redraw_locked();
            return line;
        }
        if (quit) {
            return std::nullopt;
        }
    }
}
#endif

void Terminal::set_prompt(std::string prompt, std::size_t prompt_width) {
    std::scoped_lock lock(mutex_);
    prompt_ = std::move(prompt);
    prompt_width_ = prompt_width;
    if (interactive_) {
        redraw_locked();
    }
}

void Terminal::print(std::string_view line) {
    std::scoped_lock lock(mutex_);
    if (!interactive_) {
        std::string out(line);
        out += '\n';
        write(out);
        return;
    }
    std::string out = clear_line_locked();
    out += line;
    out += "\r\n";
    write(out);
    redraw_locked();
}

std::optional<std::string> Terminal::read_line() {
    return interactive_ ? read_line_interactive() : read_line_plain();
}

std::optional<std::string> Terminal::read_line_plain() {
    std::string line;
    if (!std::getline(std::cin, line)) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

bool Terminal::on_char(char32_t cp, bool& quit) {
    std::scoped_lock lock(mutex_);
    switch (cp) {
    case '\r':
    case '\n':
        return true;
    case key_ctrl_c:
        quit = true;
        return false;
    case key_ctrl_d:
    case key_ctrl_z:
        quit = buffer_.empty();
        return false;
    case key_backspace:
    case key_delete:
        text::pop_back(buffer_);
        break;
    case key_ctrl_u:
        buffer_.clear();
        break;
    case key_ctrl_w:
        while (!buffer_.empty() && buffer_.back() == ' ') {
            buffer_.pop_back();
        }
        while (!buffer_.empty() && buffer_.back() != ' ') {
            text::pop_back(buffer_);
        }
        break;
    case '\t':
        cp = ' ';
        [[fallthrough]];
    default:
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) {
            return false;
        }
        {
            std::string encoded;
            text::append(encoded, cp);
            if (buffer_.size() + encoded.size() > max_text_bytes) {
                return false;
            }
            buffer_ += encoded;
        }
        break;
    }
    redraw_locked();
    return false;
}

std::string Terminal::clear_line_locked() {
    if (vt_) {
        return "\r\x1b[2K";
    }
    return "\r" + std::string(width() - 1, ' ') + "\r";
}

void Terminal::redraw_locked() {
    // Show only the end of the input when it is longer than the line, so it never wraps.
    const std::size_t cols = width();
    const std::size_t room = cols > prompt_width_ + 2 ? cols - prompt_width_ - 2 : 1;
    std::string out = clear_line_locked();
    out += prompt_;
    out += text::tail(buffer_, room);
    write(out);
}

} // namespace zchat
