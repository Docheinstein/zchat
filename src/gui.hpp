#pragma once

#include "screen.hpp"

#include <cstdint>
#include <memory>

namespace zchat {

// The chat in a window: the system's web view (WebView2 on Windows, WebKit on macOS and Linux) showing the page of
// ui/index.html, with the chat's lines turned from ANSI into styled text, pictures, a list of the people in the
// chat, '@' tags, dropped images, and text size and font settings.
//
// The window must run on the main thread (see run()); the chat runs on another, and talks to it as to a terminal.
class Gui : public Screen {
public:
    Gui();
    ~Gui() override;
    Gui(const Gui&) = delete;
    Gui& operator=(const Gui&) = delete;

    // Whether a window can be opened here (on Linux, whether there is a display).
    static bool available();

    // Shows what the header says: the UDP port of the chat.
    void set_info(std::uint16_t port);

    // Shows the window until it is closed (by the user, or by close()). On the main thread.
    void run();

    // Closes the window. Thread-safe.
    void close();

    bool colors() const override {
        return true;
    }
    void set_prompt(std::string prompt, std::size_t prompt_width) override;
    void print(std::string_view line) override;
    std::optional<std::string> read_line() override;
    void set_mentions(std::function<std::vector<Mention>()> source) override;
    void set_self(std::function<Mention()> source) override;
    void set_avatars(std::function<std::optional<std::string>(std::string_view hash)> source) override;
    void set_rewriter(std::function<std::optional<std::string>(std::string_view)> rewriter) override;
    void bell() override;
    bool show_image(std::string_view line, int width, int height, const std::vector<Frame>& frames) override;
    bool show_file(std::string_view line, std::size_t index, std::string_view name, std::string_view size) override;
    void interrupt() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zchat
