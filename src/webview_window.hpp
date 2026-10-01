#pragma once

#include <functional>
#include <memory>
#include <string>

namespace zchat {

// A window of the system's web view, through the webview library: what the zchat window (see gui.hpp) uses of it.
//
// webview's header does not build as C++23 with Clang (as on Linux), so only webview_window.cpp includes it, and is
// built as C++20; this header keeps it away from the rest of zchat.
class WebviewWindow {
public:
    WebviewWindow();
    ~WebviewWindow();
    WebviewWindow(const WebviewWindow&) = delete;
    WebviewWindow& operator=(const WebviewWindow&) = delete;

    void set_title(const std::string& title);
    void set_size(int width, int height);
    void set_min_size(int width, int height);
    void set_html(const std::string& html);

    // Makes window.name(...) in the page call function, with its arguments as a JSON array; what it returns, JSON
    // too, is what the page's promise gives. Called on the window's thread.
    void bind(const std::string& name, std::function<std::string(const std::string& args)> function);

    // Runs work on the window's thread, from any thread.
    void dispatch(std::function<void()> work);

    // Runs JavaScript in the page. On the window's thread.
    void eval(const std::string& js);

    // Shows the window until it is closed (by the user, or by terminate()). On the main thread.
    void run();

    // Ends run(). On the window's thread.
    void terminate();

    // The file of the latest drag on the window, its full path; empty when none. Where the page cannot have the
    // file: WebKitGTK gives a page no file dropped on it, only GTK has it. On the window's thread.
    std::string dropped_file() const;

    // The window of the system: an HWND on Windows, a GtkWindow* on Linux, an NSWindow* on macOS; or null.
    void* native_window();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zchat
