#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace zchat {

// Console I/O that lets other threads print lines while the user is typing: the line being edited is kept at
// the bottom and redrawn after every printed line.
//
// When stdin or stdout is not a terminal (e.g. piped), it falls back to plain line-by-line I/O.
class Terminal {
public:
    Terminal();
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    // Whether ANSI colors and cursor control can be used.
    bool colors() const {
        return vt_;
    }

    // Sets the prompt; prompt_width is its visible width (without escape sequences).
    void set_prompt(std::string prompt, std::size_t prompt_width);

    // Prints a full line above the input line. Thread-safe.
    void print(std::string_view line);

    // Reads the next line typed by the user, or nullopt on end of input (Ctrl+D, Ctrl+C, closed stdin).
    std::optional<std::string> read_line();

private:
    struct Platform;

    std::optional<std::string> read_line_plain();
    std::optional<std::string> read_line_interactive();
    // Handles one typed code point (or one of the key_* values); returns true when the line is complete.
    bool on_char(char32_t cp, bool& quit);
    // Hands out the completed line, remembering it in the history.
    std::string take_line();
    // Replaces the input with an older (step -1) or newer (step +1) line from the history.
    void recall_locked(int step);
    void write(std::string_view data);
    void redraw_locked();
    std::string clear_line_locked();
    std::size_t width() const;

    std::unique_ptr<Platform> platform_;
    bool interactive_ = false;
    bool vt_ = false;

    std::mutex mutex_;
    std::string prompt_;
    std::size_t prompt_width_ = 0;
    std::string buffer_;

    // The lines sent this session, oldest first. history_pos_ is the entry shown by Up/Down; it equals
    // history_.size() while editing a new line, which is kept in draft_ while browsing.
    std::deque<std::string> history_;
    std::size_t history_pos_ = 0;
    std::string draft_;
};

} // namespace zchat
