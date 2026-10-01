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
};

WebviewWindow::WebviewWindow() : impl_(std::make_unique<Impl>()) {}

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

void* WebviewWindow::native_window() {
    const auto window = impl_->view.window();
    return window.ok() ? window.value() : nullptr;
}

} // namespace zchat
