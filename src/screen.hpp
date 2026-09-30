#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zchat {

// Where the chat is shown and typed: the terminal (see Terminal) or a window (see Gui). The chat, its commands and
// games only talk to this, so they work the same in both.
//
// Lines are text with ANSI escape sequences for their look (SGR only: colors, bold, italic, underline,
// strikethrough), as a terminal shows them; a window turns them into its own styles.
class Screen {
public:
    virtual ~Screen() = default;

    // Whether ANSI colors and styles can be used.
    virtual bool colors() const = 0;

    // Sets the prompt; prompt_width is its visible width (without escape sequences).
    virtual void set_prompt(std::string prompt, std::size_t prompt_width) = 0;

    // Prints a full line above the input line; it can span several lines, separated by '\n'. Thread-safe.
    virtual void print(std::string_view line) = 0;

    // Reads the next line typed by the user, or nullopt at the end (the user quits or closes the window) or after
    // interrupt().
    virtual std::optional<std::string> read_line() = 0;

    // Someone who can be tagged by typing '@': the name inserted, and the ANSI SGR parameters it is shown with in
    // the list (e.g. "38;2;R;G;B"), empty for plain text.
    struct Mention {
        std::string name;
        std::string style;
    };

    // Sets where the list shown after typing '@' gets its names from. The source is called with the screen
    // locked, so it must not print.
    virtual void set_mentions(std::function<std::vector<Mention>()> source) = 0;

    // Sets what may rewrite the input line once the keys typed so far are handled and no more are waiting, as after
    // a paste or a file dropped on the window: it gets the line and returns its replacement, or nullopt to keep it.
    // It is called with the screen locked, so it must not print.
    virtual void set_rewriter(std::function<std::optional<std::string>(std::string_view)> rewriter) = 0;

    // Plays the notification sound. Thread-safe.
    virtual void bell() = 0;

    // Makes read_line() return nullopt right away, now and from then on. Thread-safe.
    virtual void interrupt() = 0;
};

} // namespace zchat
