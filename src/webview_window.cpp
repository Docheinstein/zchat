#include "webview_window.hpp"

#include <utility>

#ifdef __linux__
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#endif

// The web view library: third party code, whose warnings are not ours to fix.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <webview/webview.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifdef WEBVIEW_GTK
#include "zchat_desktop.hpp"
#include "zchat_icon_png.hpp"
#endif

namespace zchat {

#ifdef WEBVIEW_GTK
namespace {

    // Gives path content, unless it already has it. Quietly: what fails only costs zchat its icon on the desktop.
    void write_if_changed(const std::filesystem::path& path, std::string_view content) {
        if (std::ifstream in {path, std::ios::binary}) {
            if (std::string(std::istreambuf_iterator<char>(in), {}) == content) {
                return;
            }
        }
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream(path, std::ios::binary | std::ios::trunc).write(content.data(), std::streamsize(content.size()));
    }

    // The desktop (GNOME, since version 43) shows the icon of a window only through the .desktop file of its
    // application, found by the name of the program: zchat names itself, and puts its .desktop file (made from
    // extras/zchat.desktop.in, for the zchat that runs, even one just built by `just run`) and its icon among the
    // user's own applications. Before the window is made, which is when the desktop looks for them.
    bool join_desktop() {
        if (!g_get_prgname()) {
            g_set_prgname("zchat");
        }
        std::filesystem::path data;
        if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
            data = xdg;
        } else if (const char* home = std::getenv("HOME"); home && *home) {
            data = std::filesystem::path(home) / ".local" / "share";
        } else {
            return false;
        }
        std::error_code ec;
        const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec) {
            return false;
        }
        write_if_changed(data / "icons" / "hicolor" / "256x256" / "apps" / "zchat.png",
                         {reinterpret_cast<const char*>(zchat_icon_png), zchat_icon_png_size});
        // Exec="...": inside the quotes, ", `, $ and \ are escaped with a \, which the file escapes again.
        std::string quoted;
        for (const char c : exe.string()) {
            if (c == '"' || c == '`' || c == '$') {
                quoted += "\\\\";
            } else if (c == '\\') {
                quoted += "\\\\\\";
            }
            quoted += c;
        }
        std::string entry(reinterpret_cast<const char*>(zchat_desktop), zchat_desktop_size);
        constexpr std::string_view placeholder = "@ZCHAT_EXE@";
        if (const auto at = entry.find(placeholder); at != std::string::npos) {
            entry.replace(at, placeholder.size(), quoted);
        }
        write_if_changed(data / "applications" / "zchat.desktop", entry);
        return true;
    }

} // namespace
#endif

struct WebviewWindow::Impl {
#ifdef WEBVIEW_GTK
    bool on_desktop = join_desktop();
#endif
    webview::webview view {false, nullptr};
    std::string dropped_file;
};

WebviewWindow::WebviewWindow() :
    impl_(std::make_unique<Impl>()) {
#if defined(WEBVIEW_GTK) && GTK_MAJOR_VERSION == 3
    // GTK asks for what is dragged as it comes over the web view, and again for the drop; this runs before
    // WebKitGTK takes it (which then keeps the file from the page).
    const auto received =
        +[](GtkWidget*, GdkDragContext*, gint, gint, GtkSelectionData* data, guint, guint, gpointer impl) {
            auto& dropped = static_cast<Impl*>(impl)->dropped_file;
            dropped.clear();
            gchar** uris = gtk_selection_data_get_uris(data);
            if (!uris) {
                return;
            }
            for (gchar** uri = uris; *uri && dropped.empty(); ++uri) {
                // Only a file of this computer has a path (not, say, a link dragged from a browser).
                if (gchar* path = g_filename_from_uri(*uri, nullptr, nullptr)) {
                    dropped = path;
                    g_free(path);
                }
            }
            g_strfreev(uris);
        };
    if (const auto web_view = impl_->view.browser_controller(); web_view.ok()) {
        g_signal_connect(web_view.value(), "drag-data-received", G_CALLBACK(received), impl_.get());
    }

    // The icon of zchat (extras/icons), built into the executable: for the window, and the popups (see popup.hpp).
    GdkPixbufLoader* const loader = gdk_pixbuf_loader_new();
    const bool written = gdk_pixbuf_loader_write(loader, zchat_icon_png, zchat_icon_png_size, nullptr);
    if (gdk_pixbuf_loader_close(loader, nullptr) && written) {
        GdkPixbuf* const icon = gdk_pixbuf_loader_get_pixbuf(loader);
        gtk_window_set_default_icon(icon);
        if (const auto window = impl_->view.window(); window.ok()) {
            gtk_window_set_icon(GTK_WINDOW(window.value()), icon);
        }
    }
    g_object_unref(loader);
#endif
}

WebviewWindow::~WebviewWindow() = default;

void WebviewWindow::set_title(const std::string& title) {
    impl_->view.set_title(title);
}

void WebviewWindow::set_size(int width, int height) {
    impl_->view.set_size(width, height, WEBVIEW_HINT_NONE);
}

void WebviewWindow::set_min_size(int width, int height) {
    impl_->view.set_size(width, height, WEBVIEW_HINT_MIN);
}

void WebviewWindow::set_html(const std::string& html) {
    impl_->view.set_html(html);
}

void WebviewWindow::bind(const std::string& name, std::function<std::string(const std::string& args)> function) {
    impl_->view.bind(name, std::move(function));
}

void WebviewWindow::dispatch(std::function<void()> work) {
    impl_->view.dispatch(std::move(work));
}

void WebviewWindow::eval(const std::string& js) {
    impl_->view.eval(js);
}

void WebviewWindow::run() {
    impl_->view.run();
}

void WebviewWindow::terminate() {
    impl_->view.terminate();
}

std::string WebviewWindow::dropped_file() const {
    return impl_->dropped_file;
}

void* WebviewWindow::native_window() {
    const auto window = impl_->view.window();
    return window.ok() ? window.value() : nullptr;
}

} // namespace zchat
