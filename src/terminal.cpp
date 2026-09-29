#include "terminal.hpp"

#include "protocol.hpp"
#include "text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace zchat {

namespace {

    constexpr char32_t key_ctrl_a = 0x01;
    constexpr char32_t key_ctrl_c = 0x03;
    constexpr char32_t key_ctrl_d = 0x04;
    constexpr char32_t key_ctrl_e = 0x05;
    constexpr char32_t key_backspace = 0x08;
    constexpr char32_t key_ctrl_u = 0x15;
    constexpr char32_t key_ctrl_w = 0x17;
    constexpr char32_t key_ctrl_z = 0x1A;
    constexpr char32_t key_escape = 0x1B;
    constexpr char32_t key_delete = 0x7F; // what most terminals send for Backspace
    // Keys without a character, given values past the last Unicode code point.
    constexpr char32_t key_up = 0x110000;
    constexpr char32_t key_down = 0x110001;
    constexpr char32_t key_left = 0x110002;
    constexpr char32_t key_right = 0x110003;
    constexpr char32_t key_home = 0x110004;
    constexpr char32_t key_end = 0x110005;
    constexpr char32_t key_forward_delete = 0x110006; // the Delete key

    // How many sent lines Up/Down can go back to.
    constexpr std::size_t max_history = 100;
    // How many names the '@' list shows at once; it scrolls to keep the highlighted one in view.
    constexpr std::size_t max_mention_rows = 6;

    std::string lowercase(std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        return out;
    }

    bool is_continuation(char c) {
        return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
    }

    // The start of the code point before pos.
    std::size_t prev_boundary(std::string_view s, std::size_t pos) {
        if (pos == 0) {
            return 0;
        }
        --pos;
        while (pos > 0 && is_continuation(s[pos])) {
            --pos;
        }
        return pos;
    }

    // The start of the code point after the one at pos.
    std::size_t next_boundary(std::string_view s, std::size_t pos) {
        if (pos >= s.size()) {
            return s.size();
        }
        ++pos;
        while (pos < s.size() && is_continuation(s[pos])) {
            ++pos;
        }
        return pos;
    }

    std::size_t count_chars(std::string_view s) {
        return static_cast<std::size_t>(std::ranges::count_if(s, [](char c) {
            return !is_continuation(c);
        }));
    }

    // The byte offset of the code point number index (or s.size() past the end).
    std::size_t char_offset(std::string_view s, std::size_t index) {
        std::size_t pos = 0;
        while (index > 0 && pos < s.size()) {
            pos = next_boundary(s, pos);
            --index;
        }
        return pos;
    }

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
    // Set by interrupt(), to wake up a read waiting for a key.
    HANDLE wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
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
    if (p.wake) {
        CloseHandle(p.wake);
    }
}

void Terminal::interrupt() {
    interrupted_ = true;
    if (platform_->wake) {
        SetEvent(platform_->wake);
    }
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

void Terminal::bell() {
    // The system sound: the console bell is silent in some hosts.
    MessageBeep(MB_OK);
}

std::optional<std::string> Terminal::read_line_interactive() {
    auto& p = *platform_;
    while (true) {
        // A paste (or a dropped file) arrives as many keys at once: before waiting for more, the rewriter sees the
        // line with all of them in.
        if (DWORD pending = 0; GetNumberOfConsoleInputEvents(p.in, &pending) && pending == 0) {
            std::scoped_lock lock(mutex_);
            rewrite_locked();
        }
        if (p.wake) {
            const HANDLE waits[] = {p.wake, p.in};
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
                return std::nullopt;
            }
        }
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
        if (!key.bKeyDown) {
            continue;
        }
        const auto unit = static_cast<char16_t>(key.uChar.UnicodeChar);
        char32_t cp = unit;
        if (unit == 0) {
            switch (key.wVirtualKeyCode) {
            case VK_UP:
                cp = key_up;
                break;
            case VK_DOWN:
                cp = key_down;
                break;
            case VK_LEFT:
                cp = key_left;
                break;
            case VK_RIGHT:
                cp = key_right;
                break;
            case VK_HOME:
                cp = key_home;
                break;
            case VK_END:
                cp = key_end;
                break;
            case VK_DELETE:
                cp = key_forward_delete;
                break;
            default:
                continue;
            }
        } else if (unit >= 0xD800 && unit <= 0xDBFF) {
            p.high_surrogate = unit;
            continue;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (p.high_surrogate == 0) {
                continue;
            }
            cp = 0x10000 + ((static_cast<char32_t>(p.high_surrogate) - 0xD800) << 10) + (unit - 0xDC00);
            p.high_surrogate = 0;
        }
        for (WORD i = 0; i < (key.wRepeatCount ? key.wRepeatCount : 1); ++i) {
            bool quit = false;
            if (on_char(cp, quit)) {
                return take_line();
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
    // A pipe written by interrupt(), to wake up a read waiting for a key.
    int wake[2] = {-1, -1};
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
        if (pipe2(platform_->wake, O_CLOEXEC) != 0) {
            platform_->wake[0] = platform_->wake[1] = -1;
        }
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
    for (const int fd : platform_->wake) {
        if (fd >= 0) {
            close(fd);
        }
    }
}

void Terminal::interrupt() {
    interrupted_ = true;
    if (platform_->wake[1] >= 0) {
        const char byte = 0;
        [[maybe_unused]] const auto n = ::write(platform_->wake[1], &byte, 1);
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

void Terminal::bell() {
    std::scoped_lock lock(mutex_);
    write("\a");
}

namespace {

    // Reads one byte, waiting at most timeout_ms (or forever if negative). Returns -1 on EOF/error/timeout, or
    // when wake_fd (if valid) becomes readable.
    int read_byte(int wake_fd, int timeout_ms = -1) {
        while (true) {
            pollfd fds[] = {{STDIN_FILENO, POLLIN, 0}, {wake_fd, POLLIN, 0}};
            const int ready = poll(fds, wake_fd >= 0 ? 2 : 1, timeout_ms);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready <= 0 || fds[1].revents != 0) {
                return -1;
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

    // Reads the rest of an escape sequence (arrow keys, function keys, ...). Returns one of the key_* values, or
    // 0 for the keys zchat does not use.
    char32_t read_escape_sequence(int wake_fd) {
        int c = read_byte(wake_fd, 30);
        if (c < 0) {
            // Nothing follows: the Escape key itself.
            return key_escape;
        }
        if (c != '[' && c != 'O') {
            return 0;
        }
        std::string params;
        do {
            c = read_byte(wake_fd, 30);
            if (c >= 0 && !(c >= 0x40 && c <= 0x7E)) {
                params += static_cast<char>(c);
            }
        } while (c >= 0 && !(c >= 0x40 && c <= 0x7E));
        // Plain keys only: "ESC [ A" or "ESC O A", not modified ones like "ESC [ 1 ; 5 A".
        if (params.empty()) {
            switch (c) {
            case 'A':
                return key_up;
            case 'B':
                return key_down;
            case 'C':
                return key_right;
            case 'D':
                return key_left;
            case 'H':
                return key_home;
            case 'F':
                return key_end;
            default:
                return 0;
            }
        }
        // "ESC [ 3 ~" and friends, which differ between terminals for Home and End.
        if (c == '~') {
            if (params == "1" || params == "7") {
                return key_home;
            }
            if (params == "4" || params == "8") {
                return key_end;
            }
            if (params == "3") {
                return key_forward_delete;
            }
        }
        return 0;
    }

} // namespace

std::optional<std::string> Terminal::read_line_interactive() {
    const int wake_fd = platform_->wake[0];
    while (true) {
        // A paste (or a dropped file) arrives as many keys at once: before waiting for more, the rewriter sees the
        // line with all of them in.
        if (pollfd pending {STDIN_FILENO, POLLIN, 0}; poll(&pending, 1, 0) == 0) {
            std::scoped_lock lock(mutex_);
            rewrite_locked();
        }
        int c = read_byte(wake_fd);
        if (c < 0) {
            return std::nullopt;
        }
        char32_t cp = static_cast<char32_t>(c);
        if (c == 0x1B) {
            cp = read_escape_sequence(wake_fd);
            if (cp == 0) {
                continue;
            }
        } else if (c >= 0x80) {
            // Collect the rest of a UTF-8 sequence.
            std::string bytes(1, static_cast<char>(c));
            const int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
            for (int i = 1; i < len; ++i) {
                const int cc = read_byte(wake_fd, 30);
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
            return take_line();
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
    for (const char c : line) {
        // In raw mode, a line feed alone may not go back to the start of the line.
        out += c == '\n' ? std::string_view("\r\n") : std::string_view(&c, 1);
    }
    out += "\r\n";
    write(out);
    redraw_locked();
}

std::optional<std::string> Terminal::read_line() {
    if (interrupted_) {
        return std::nullopt;
    }
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

void Terminal::set_mentions(std::function<std::vector<Mention>()> source) {
    std::scoped_lock lock(mutex_);
    mention_source_ = std::move(source);
}

std::vector<Terminal::Mention> Terminal::mention_matches_locked() const {
    std::vector<Mention> matches;
    if (!mention_start_ || !mention_source_) {
        return matches;
    }
    const std::size_t from = *mention_start_ + 1;
    const std::string typed = lowercase(std::string_view(buffer_).substr(from, cursor_ - from));
    for (auto& mention : mention_source_()) {
        if (lowercase(mention.name).starts_with(typed)) {
            matches.push_back(std::move(mention));
        }
    }
    return matches;
}

void Terminal::update_mention_locked(char32_t typed) {
    // The list needs cursor movements to be drawn below the input line.
    if (!vt_ || !mention_source_) {
        return;
    }
    // An '@' starting a word opens the list; one inside a word, like in an email address, does not.
    if (typed == '@' && cursor_ > 0 && buffer_[cursor_ - 1] == '@' && (cursor_ == 1 || buffer_[cursor_ - 2] == ' ')) {
        mention_start_ = cursor_ - 1;
    } else if (mention_start_ &&
               (cursor_ <= *mention_start_ || *mention_start_ >= buffer_.size() || buffer_[*mention_start_] != '@')) {
        // The cursor went back before the '@', or the '@' was deleted.
        mention_start_.reset();
    }
    mention_selected_ = 0;
}

void Terminal::accept_mention_locked(const Mention& mention) {
    const std::size_t start = *mention_start_;
    mention_start_.reset();
    const std::string inserted = "@" + mention.name + " ";
    if (buffer_.size() - (cursor_ - start) + inserted.size() > max_text_bytes) {
        return;
    }
    buffer_.replace(start, cursor_ - start, inserted);
    cursor_ = start + inserted.size();
}

bool Terminal::on_char(char32_t cp, bool& quit) {
    std::scoped_lock lock(mutex_);
    // While the '@' list is shown, Up/Down move in it, Enter/Tab pick the name and Escape closes it.
    if (const auto matches = mention_matches_locked(); !matches.empty()) {
        const std::size_t n = matches.size();
        switch (cp) {
        case key_up:
            mention_selected_ = (mention_selected_ % n + n - 1) % n;
            redraw_locked();
            return false;
        case key_down:
            mention_selected_ = (mention_selected_ + 1) % n;
            redraw_locked();
            return false;
        case '\r':
        case '\n':
        case '\t':
            accept_mention_locked(matches[mention_selected_ % n]);
            redraw_locked();
            return false;
        case key_escape:
            mention_start_.reset();
            redraw_locked();
            return false;
        default:
            break;
        }
    }
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
    case key_delete: {
        const std::size_t start = prev_boundary(buffer_, cursor_);
        buffer_.erase(start, cursor_ - start);
        cursor_ = start;
        break;
    }
    case key_forward_delete:
        buffer_.erase(cursor_, next_boundary(buffer_, cursor_) - cursor_);
        break;
    case key_left:
        cursor_ = prev_boundary(buffer_, cursor_);
        break;
    case key_right:
        cursor_ = next_boundary(buffer_, cursor_);
        break;
    case key_home:
    case key_ctrl_a:
        cursor_ = 0;
        break;
    case key_end:
    case key_ctrl_e:
        cursor_ = buffer_.size();
        break;
    case key_ctrl_u:
        buffer_.clear();
        cursor_ = 0;
        break;
    case key_up:
        recall_locked(-1);
        break;
    case key_down:
        recall_locked(+1);
        break;
    case key_ctrl_w: {
        // Deletes the word before the cursor, and the spaces after it.
        std::size_t start = cursor_;
        while (start > 0 && buffer_[start - 1] == ' ') {
            --start;
        }
        while (start > 0 && buffer_[start - 1] != ' ') {
            start = prev_boundary(buffer_, start);
        }
        buffer_.erase(start, cursor_ - start);
        cursor_ = start;
        break;
    }
    case '\t':
        cp = ' ';
        [[fallthrough]];
    default:
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp > 0x10FFFF) {
            return false;
        }
        {
            std::string encoded;
            text::append(encoded, cp);
            if (buffer_.size() + encoded.size() > max_text_bytes) {
                return false;
            }
            buffer_.insert(cursor_, encoded);
            cursor_ += encoded.size();
        }
        break;
    }
    update_mention_locked(cp);
    redraw_locked();
    return false;
}

std::string Terminal::take_line() {
    std::scoped_lock lock(mutex_);
    std::string line = std::move(buffer_);
    buffer_.clear();
    cursor_ = 0;
    view_start_ = 0;
    draft_.clear();
    mention_start_.reset();
    // Blank lines and repeats of the previous line are not worth an extra Up press.
    if (line.find_first_not_of(' ') != std::string::npos && (history_.empty() || history_.back() != line)) {
        history_.push_back(line);
        if (history_.size() > max_history) {
            history_.pop_front();
        }
    }
    history_pos_ = history_.size();
    redraw_locked();
    return line;
}

void Terminal::set_rewriter(std::function<std::optional<std::string>(std::string_view)> rewriter) {
    std::scoped_lock lock(mutex_);
    rewriter_ = std::move(rewriter);
}

void Terminal::rewrite_locked() {
    if (!rewriter_ || buffer_.empty()) {
        return;
    }
    if (auto line = rewriter_(buffer_); line && *line != buffer_ && line->size() <= max_text_bytes) {
        buffer_ = std::move(*line);
        cursor_ = buffer_.size();
        mention_start_.reset();
        redraw_locked();
    }
}

void Terminal::recall_locked(int step) {
    if ((step < 0 && history_pos_ == 0) || (step > 0 && history_pos_ >= history_.size())) {
        return;
    }
    if (history_pos_ == history_.size()) {
        draft_ = buffer_;
    }
    history_pos_ = step < 0 ? history_pos_ - 1 : history_pos_ + 1;
    buffer_ = history_pos_ == history_.size() ? draft_ : history_[history_pos_];
    cursor_ = buffer_.size();
    mention_start_.reset();
}

std::string Terminal::clear_line_locked() {
    if (vt_) {
        // Down to the end of the screen, for the '@' list below the input line.
        return "\r\x1b[J";
    }
    return "\r" + std::string(width() - 1, ' ') + "\r";
}

void Terminal::redraw_locked() {
    // When the input is longer than the line, show the part around the cursor, so it never wraps. The shown part
    // only scrolls when the cursor would leave it.
    const std::size_t cols = width();
    const std::size_t room = cols > prompt_width_ + 2 ? cols - prompt_width_ - 2 : 1;
    const std::size_t total = count_chars(buffer_);
    const std::size_t cursor = count_chars(std::string_view(buffer_).substr(0, cursor_));
    if (cursor < view_start_) {
        view_start_ = cursor;
    } else if (cursor > view_start_ + room) {
        view_start_ = cursor - room;
    }
    // Scroll back when text was deleted, so the line stays full.
    view_start_ = std::min(view_start_, total > room ? total - room : 0);

    const std::size_t begin = char_offset(buffer_, view_start_);
    const std::size_t end = char_offset(buffer_, view_start_ + room);
    const std::string_view before_cursor = std::string_view(buffer_).substr(begin, cursor_ - begin);
    std::string out = clear_line_locked();
    out += prompt_;
    out += std::string_view(buffer_).substr(begin, end - begin);
    const std::size_t back = count_chars(std::string_view(buffer_).substr(cursor_, end - cursor_));
    if (const auto matches = mention_matches_locked(); !matches.empty()) {
        // The '@' list goes below the input line, the highlighted name marked with '>'. At the bottom of the
        // screen, the line feeds scroll it up to make room.
        const std::size_t selected = mention_selected_ % matches.size();
        const std::size_t rows = std::min(matches.size(), max_mention_rows);
        const std::size_t first = selected >= rows ? selected - rows + 1 : 0;
        const std::size_t name_room = cols > 5 ? cols - 5 : 1;
        for (std::size_t i = first; i < first + rows; ++i) {
            const Mention& m = matches[i];
            const std::string_view name = std::string_view(m.name).substr(0, char_offset(m.name, name_room));
            out += i == selected ? "\r\n \x1b[1m>\x1b[0m " : "\r\n   ";
            out += m.style.empty() ? std::string(name) : std::format("\x1b[{}m{}\x1b[0m", m.style, name);
        }
        out += std::format("\x1b[{}A\r", rows);
        const std::size_t column = prompt_width_ + cursor - view_start_;
        if (column > 0) {
            out += std::format("\x1b[{}C", column);
        }
    } else if (back > 0) {
        if (vt_) {
            out += std::format("\x1b[{}D", back);
        } else {
            // Without escape sequences, writing the start of the line again leaves the cursor at the right place.
            out += '\r';
            out += prompt_;
            out += before_cursor;
        }
    }
    write(out);
}

} // namespace zchat
