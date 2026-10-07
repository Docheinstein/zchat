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
        // Their avatar's hash (see Chat::set_avatar()), in hex; empty for none.
        std::string avatar = {};
    };

    // Sets where the list shown after typing '@' gets its names from. The source is called with the screen
    // locked, so it must not print.
    virtual void set_mentions(std::function<std::vector<Mention>()> source) = 0;

    // Sets where our own name and its style come from, for screens that show them (the window, above the channels).
    // Called with the screen locked, so it must not print.
    virtual void set_self(std::function<Mention()> /*source*/) {}

    // Sets where the avatars come from, for screens that show them: by hash, the picture (see
    // image::encode_avatar()), or nullopt when it is not here (yet). Thread-safe.
    virtual void set_avatars(std::function<std::optional<std::string>(std::string_view hash)> /*source*/) {}

    // A channel, for screens that list them (the window): see Chat::channels().
    struct ChannelItem {
        std::string name;
        bool is_public = true;
        bool member = false;
        bool current = false;
        bool unread = false;
        bool owner = false;
    };
    virtual void set_channels(std::function<std::vector<ChannelItem>()> /*source*/) {}

    // Who is in a channel, and who could be added to it (see Chat::channel_people()), for screens that show them.
    struct ChannelPeople {
        std::vector<Mention> members;
        std::vector<Mention> others;
        std::size_t away = 0;
    };
    virtual void set_channel_people(std::function<ChannelPeople(std::string_view channel)> /*source*/) {}

    // Shows the state of a game in a window of its own (see the dice game), where it can: state is JSON, as the
    // game makes it. Returns false where it cannot (a terminal), where the game is played with commands. Thread-safe.
    virtual bool show_game(std::string_view /*game*/, std::string_view /*state*/) {
        return false;
    }

    // Whether what the games say is kept apart from the chat, in a channel of its own (see channel::games_log), as a
    // window does; or else shown in the chat, as a terminal does.
    virtual bool games_apart() const {
        return false;
    }

    // Empties what is shown (another channel is joined), where it can: returns false where it cannot (a terminal).
    // Thread-safe.
    virtual bool clear() {
        return false;
    }

    // Sets what may rewrite the input line once the keys typed so far are handled and no more are waiting, as after
    // a paste or a file dropped on the window: it gets the line and returns its replacement, or nullopt to keep it.
    // It is called with the screen locked, so it must not print.
    virtual void set_rewriter(std::function<std::optional<std::string>(std::string_view)> rewriter) = 0;

    // Plays the notification sound. Thread-safe.
    virtual void bell() = 0;

    // A frame of a picture: an image file (its MIME type and its bytes in base64), shown for delay_ms milliseconds
    // when there are several (an animation).
    struct Frame {
        std::string_view mime;
        std::string_view base64;
        int delay_ms = 0;
    };

    // Shows a real picture, after a line (e.g. who sent it), if this screen can. Returns false where pictures cannot
    // be shown (a terminal), for them to be drawn with characters instead. Thread-safe.
    virtual bool show_image(std::string_view line, int width, int height, const std::vector<Frame>& frames) {
        (void)line;
        (void)width;
        (void)height;
        (void)frames;
        return false;
    }

    // Shows a received file, after a line (e.g. who sent it), with a way to download it: typing "/save INDEX". Returns
    // false where that cannot be offered but by printing the command (a terminal). Thread-safe.
    virtual bool show_file(std::string_view line, std::size_t index, std::string_view name, std::string_view size) {
        (void)line;
        (void)index;
        (void)name;
        (void)size;
        return false;
    }

    // Shows a chat message (line, as print() would) with a way to reply quoting it: reply_quote is what to put before
    // the reply (see markup::quote()). When the message quotes another one, quote is that quote's line (the name, then
    // the text), shown with it. Returns false where messages cannot be quoted but with /quote (a terminal), for it to
    // be printed instead. Thread-safe.
    virtual bool show_message(std::string_view line, std::optional<std::string_view> quote,
                              std::string_view reply_quote) {
        (void)line;
        (void)quote;
        (void)reply_quote;
        return false;
    }

    // A trill (/trill): brings the window to the front, shown again if it is minimized, and shakes it, where it can
    // (see popup::nudge()). Takes about half a second. Thread-safe.
    virtual void nudge() {}

    // Makes read_line() return nullopt right away, now and from then on. Thread-safe.
    virtual void interrupt() = 0;
};

} // namespace zchat
