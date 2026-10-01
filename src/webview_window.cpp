#include "webview_window.hpp"

#include <utility>

// The web view library: third party code, whose warnings are not ours to fix.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <webview/webview.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace zchat {

struct WebviewWindow::Impl {
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
