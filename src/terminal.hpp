#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

    // Prints a full line above the input line; it can span several lines, separated by '\n'. Thread-safe.
    void print(std::string_view line);

    // Reads the next line typed by the user, or nullopt on end of input (Ctrl+D, Ctrl+C, closed stdin) or after
    // interrupt().
    std::optional<std::string> read_line();

    // Someone who can be tagged by typing '@': the name inserted, and the ANSI SGR parameters it is shown with in
    // the list (e.g. "38;2;R;G;B"), empty for plain text.
    struct Mention {
        std::string name;
        std::string style;
    };

    // Sets where the list shown after typing '@' gets its names from. The source is called with the terminal
    // locked, so it must not print.
    void set_mentions(std::function<std::vector<Mention>()> source);

    // Sets what may rewrite the input line once the keys typed so far are handled and no more are waiting, as after
    // a paste or a file dropped on the window: it gets the line and returns its replacement, or nullopt to keep it.
    // It is called with the terminal locked, so it must not print.
    void set_rewriter(std::function<std::optional<std::string>(std::string_view)> rewriter);

    // Plays the notification sound. Thread-safe.
    void bell();

    // Makes read_line() return nullopt right away, now and from then on. Thread-safe. Without a terminal (plain
    // line-by-line input) it cannot stop a read in progress, only the following ones.
    void interrupt();

private:
    struct Platform;

    // The names matching what was typed after the '@', when the list is open.
    std::vector<Mention> mention_matches_locked() const;
    // Opens or closes the list after the input changed; typed is the key that changed it.
    void update_mention_locked(char32_t typed);
    // Replaces the '@' and what was typed after it with the chosen name.
    void accept_mention_locked(const Mention& mention);

    std::optional<std::string> read_line_plain();
    std::optional<std::string> read_line_interactive();
    // Handles one typed code point (or one of the key_* values); returns true when the line is complete.
    bool on_char(char32_t cp, bool& quit);
    // Hands out the completed line, remembering it in the history.
    std::string take_line();
    // Lets the rewriter change the input line, when no more keys are waiting.
    void rewrite_locked();
    // Replaces the input with an older (step -1) or newer (step +1) line from the history.
    void recall_locked(int step);
    void write(std::string_view data);
    void redraw_locked();
    std::string clear_line_locked();
    std::size_t width() const;

    std::unique_ptr<Platform> platform_;
    bool interactive_ = false;
    bool vt_ = false;
    std::atomic<bool> interrupted_ {false};

    std::mutex mutex_;
    std::string prompt_;
    std::size_t prompt_width_ = 0;
    std::string buffer_;
    // Byte offset of the cursor in buffer_, always at the start of a code point.
    std::size_t cursor_ = 0;
    // The first code point of buffer_ shown, when it does not fit on the line.
    std::size_t view_start_ = 0;

    // The lines sent this session, oldest first. history_pos_ is the entry shown by Up/Down; it equals
    // history_.size() while editing a new line, which is kept in draft_ while browsing.
    std::deque<std::string> history_;
    std::size_t history_pos_ = 0;
    std::string draft_;

    std::function<std::vector<Mention>()> mention_source_;
    std::function<std::optional<std::string>(std::string_view)> rewriter_;
    // Byte offset in buffer_ of the '@' the list is open for, and the highlighted entry of the list.
    std::optional<std::size_t> mention_start_;
    std::size_t mention_selected_ = 0;
};

} // namespace zchat
