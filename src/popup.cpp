#include "popup.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <limits>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <thread>
#include <windows.h>

#include <windowsx.h>
#define ZCHAT_POPUPS_WIN32 1
#elif defined(__linux__) && defined(ZCHAT_GUI)
#include <gtk/gtk.h>
#if GTK_MAJOR_VERSION == 3
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#define ZCHAT_POPUPS_GTK 1
#endif
#endif

namespace zchat::popup {

#if defined(ZCHAT_POPUPS_WIN32) || defined(ZCHAT_POPUPS_GTK)
namespace {

    // How fast the alert runs around, in pixels per second (at 96 DPI), and how much the picture and the window shake.
    constexpr double alert_speed = 420;
    constexpr double shake_pixels = 10;
    // The picture's size: this much of the smaller side of the screen.
    constexpr double picture_share = 0.55;

    // Something moving around an area, bouncing off its sides.
    struct Mover {
        double x = 0;
        double y = 0;
        double vx = 0;
        double vy = 0;

        // Moves for dt seconds, keeping a w x h box inside the area.
        void step(double dt, double w, double h, double left, double top, double right, double bottom) {
            x += vx * dt;
            y += vy * dt;
            if (x + w > right) {
                x = right - w;
                vx = -std::abs(vx);
            }
            if (x < left) {
                x = left;
                vx = std::abs(vx);
            }
            if (y + h > bottom) {
                y = bottom - h;
                vy = -std::abs(vy);
            }
            if (y < top) {
                y = top;
                vy = std::abs(vy);
            }
        }
    };

    double random(double from, double to) {
        thread_local std::mt19937 rng {std::random_device {}()};
        return from < to ? std::uniform_real_distribution<double>(from, to)(rng) : from;
    }

    // Sends it off at speed, in a random direction.
    void aim(Mover& m, double speed) {
        const double angle = random(0, 2 * std::numbers::pi);
        m.vx = speed * std::cos(angle);
        m.vy = speed * std::sin(angle);
    }

    // Somewhere random for a w x h box inside the area.
    void place(Mover& m, double w, double h, double left, double top, double right, double bottom) {
        m.x = random(left, std::max(left, right - w));
        m.y = random(top, std::max(top, bottom - h));
    }

    // Notifications (see toast()): their size and the room around them (at 96 DPI), how many at most, how long they
    // stay, and how long they take to come and go, in seconds.
    constexpr double toast_width = 360;
    constexpr double toast_height = 96;
    constexpr double toast_gap = 10;
    constexpr double toast_margin = 16;
    constexpr double toast_avatar = 40;
    constexpr double toast_close = 22;
    constexpr std::size_t max_toasts = 4;
    constexpr double toast_life = 6;
    constexpr double toast_fade = 0.2;

    // The life of a notification, frame by frame: how visible it is, and where.
    struct ToastLife {
        // Seconds since it came; and shown, not counting while the mouse is on it.
        double since = 0;
        double shown = 0;
        // Seconds since it began to go, or -1 while it stays.
        double going = -1;
        // Its top, moving toward its place in the stack; NaN before the first frame.
        double y = std::numeric_limits<double>::quiet_NaN();
        bool hovered = false;

        void close() {
            if (going < 0) {
                going = 0;
            }
        }

        bool gone() const {
            return going >= toast_fade;
        }

        // 0 to 1: faded in, then out.
        double opacity() const {
            const double in = std::min(1.0, since / toast_fade);
            const double out = going < 0 ? 1.0 : std::max(0.0, 1 - going / toast_fade);
            return std::min(in, out);
        }

        // How far right of its place it still is, as a share of its width: it slides in from the side of the screen.
        double slide() const {
            const double t = std::min(1.0, since / (toast_fade * 1.5));
            return std::pow(1 - t, 3);
        }

        void step(double dt, double target) {
            since += dt;
            shown += hovered ? 0 : dt;
            if (going >= 0) {
                going += dt;
            } else if (shown >= toast_life) {
                going = 0;
            }
            y = std::isnan(y) ? target : y + (target - y) * std::min(1.0, dt * 14);
        }
    };

    // Where the tops of n notifications of height h go, the oldest first, so that the newest is at the bottom of an
    // area ending at bottom.
    std::vector<double> toast_places(std::size_t n, double h, double gap, double margin, double bottom) {
        std::vector<double> tops(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto newer = static_cast<double>(n - 1 - i);
            tops[i] = bottom - margin - h - newer * (h + gap);
        }
        return tops;
    }

    // The first letter of a title, upper case, for the circle of a notification without a picture.
    std::string initial(std::string_view title) {
        for (std::size_t i = 0; i < title.size(); ++i) {
            const auto c = static_cast<unsigned char>(title[i]);
            if (c < 0x80) {
                if (std::isalnum(c)) {
                    return std::string(1, static_cast<char>(std::toupper(c)));
                }
                continue;
            }
            // A whole UTF-8 character.
            std::size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            return std::string(title.substr(i, n));
        }
        return "Z";
    }

    // A picture of one pixel of a color (0xRRGGBB): a circle of that color, through circle_picture().
    image::DecodedImage solid_image(std::uint32_t color) {
        image::DecodedImage solid {{static_cast<unsigned char*>(std::malloc(4)), &std::free}, 1, 1};
        if (!solid.pixels) {
            return {{nullptr, &std::free}, 0, 0};
        }
        unsigned char* p = solid.pixels.get();
        p[0] = static_cast<unsigned char>(color >> 16 & 0xFF);
        p[1] = static_cast<unsigned char>(color >> 8 & 0xFF);
        p[2] = static_cast<unsigned char>(color & 0xFF);
        p[3] = 255;
        return solid;
    }

    // The picture in a d x d circle, on the color back (0xRRGGBB), with a soft edge: RGB, row by row.
    std::vector<unsigned char> circle_picture(const image::DecodedImage& picture, int d, std::uint32_t back) {
        std::vector<unsigned char> out(static_cast<std::size_t>(d) * d * 3);
        if (!picture.pixels || picture.width <= 0 || picture.height <= 0) {
            return out;
        }
        const int sw = picture.width;
        const int sh = picture.height;
        // The middle square of the picture.
        const int side = std::min(sw, sh);
        const int ox = (sw - side) / 2;
        const int oy = (sh - side) / 2;
        const double r = d / 2.0;
        const double bk[3] = {static_cast<double>(back >> 16 & 0xFF), static_cast<double>(back >> 8 & 0xFF),
                              static_cast<double>(back & 0xFF)};
        for (int y = 0; y < d; ++y) {
            for (int x = 0; x < d; ++x) {
                const double edge = std::clamp(r - std::hypot(x + 0.5 - r, y + 0.5 - r) + 0.5, 0.0, 1.0);
                const int sx = std::clamp(ox + static_cast<int>((x + 0.5) * side / d), 0, sw - 1);
                const int sy = std::clamp(oy + static_cast<int>((y + 0.5) * side / d), 0, sh - 1);
                const unsigned char* p = picture.pixels.get() + (static_cast<std::size_t>(sy) * sw + sx) * 4;
                const double a = p[3] / 255.0 * edge;
                for (int k = 0; k < 3; ++k) {
                    out[(static_cast<std::size_t>(y) * d + x) * 3 + k] =
                        static_cast<unsigned char>(std::lround(p[k] * a + bk[k] * (1 - a)));
                }
            }
        }
        return out;
    }

} // namespace
#endif

#if defined(ZCHAT_POPUPS_WIN32)
namespace {

    using clock = std::chrono::steady_clock;

    double seconds(clock::duration d) {
        return std::chrono::duration<double>(d).count();
    }

    std::wstring widen(std::string_view s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(std::max(n, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), wide.data(), n);
        return wide;
    }

    // The work area (the screen without the taskbar) of the screen the mouse is on: where the user looks.
    RECT work_area() {
        POINT mouse {};
        GetCursorPos(&mouse);
        MONITORINFO info {};
        info.cbSize = sizeof info;
        if (!GetMonitorInfoW(MonitorFromPoint(mouse, MONITOR_DEFAULTTOPRIMARY), &info)) {
            return {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        }
        return info.rcWork;
    }

    // How big things are drawn: 1 at 96 DPI (and when Windows scales zchat itself).
    double dpi_scale() {
        HDC dc = GetDC(nullptr);
        const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        return dpi > 0 ? dpi / 96.0 : 1.0;
    }

    int px(double v) {
        return static_cast<int>(std::lround(v));
    }

    // Handles the messages of this thread's windows, then waits for more for at most ms.
    void pump(DWORD ms) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT);
    }

    // What the alert shows, and whether its STOP was pressed, for its window procedure.
    struct Alert {
        std::wstring text;
        std::wstring button;
        RECT stop {};
        HFONT font = nullptr;
        HFONT button_font = nullptr;
        bool pressed = false;
    };

    LRESULT CALLBACK alert_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* a = reinterpret_cast<Alert*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_MOUSEACTIVATE:
            // Clicked without taking the focus from what the user types in.
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            // Pressed, not clicked: it runs away before the button is let go.
            if (const POINT p {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; a && PtInRect(&a->stop, p)) {
                a->pressed = true;
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT r;
            GetClientRect(hwnd, &r);
            HBRUSH back = CreateSolidBrush(RGB(255, 243, 176));
            FillRect(dc, &r, back);
            DeleteObject(back);
            if (a) {
                SetBkMode(dc, TRANSPARENT);
                HGDIOBJ old_font = SelectObject(dc, a->font);
                SetTextColor(dc, RGB(40, 40, 40));
                RECT top {r.left + 8, r.top, r.right - 8, a->stop.top};
                DrawTextW(dc, a->text.c_str(), -1, &top, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                HBRUSH red = CreateSolidBrush(RGB(220, 38, 38));
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(150, 20, 20));
                HGDIOBJ old_brush = SelectObject(dc, red);
                HGDIOBJ old_pen = SelectObject(dc, pen);
                const int round = (a->stop.bottom - a->stop.top) / 3;
                RoundRect(dc, a->stop.left, a->stop.top, a->stop.right, a->stop.bottom, round, round);
                SelectObject(dc, old_pen);
                SelectObject(dc, old_brush);
                DeleteObject(pen);
                DeleteObject(red);
                SelectObject(dc, a->button_font);
                SetTextColor(dc, RGB(255, 255, 255));
                RECT button = a->stop;
                DrawTextW(dc, a->button.c_str(), -1, &button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dc, old_font);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    // The window classes, registered once for the whole program (windows of them can be made on any thread).
    const wchar_t* alert_class() {
        static const ATOM atom = [] {
            WNDCLASSW wc {};
            wc.lpfnWndProc = alert_proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_HAND);
            wc.lpszClassName = L"zchat_trill_alert";
            return RegisterClassW(&wc);
        }();
        (void)atom;
        return L"zchat_trill_alert";
    }

    const wchar_t* picture_class() {
        static const ATOM atom = [] {
            WNDCLASSW wc {};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"zchat_trill_picture";
            return RegisterClassW(&wc);
        }();
        (void)atom;
        return L"zchat_trill_picture";
    }

    // What a question shows, where its buttons are, and which was pressed, for its window procedure.
    struct Ask {
        std::wstring text;
        std::wstring left;
        std::wstring yes;
        std::wstring no;
        RECT yes_button {};
        RECT no_button {};
        HFONT font = nullptr;
        HFONT small_font = nullptr;
        HFONT button_font = nullptr;
        // 0 to 1: how far the background is from its yellow to its orange, as it flashes.
        double glow = 0;
        Answer answer = Answer::None;
    };

    void draw_button(HDC dc, const RECT& r, COLORREF fill, COLORREF edge, const std::wstring& label, HFONT font) {
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 2, edge);
        HGDIOBJ old_brush = SelectObject(dc, brush);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        const int round = (r.bottom - r.top) / 3;
        RoundRect(dc, r.left, r.top, r.right, r.bottom, round, round);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(pen);
        DeleteObject(brush);
        HGDIOBJ old_font = SelectObject(dc, font);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT text = r;
        DrawTextW(dc, label.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old_font);
    }

    LRESULT CALLBACK ask_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* a = reinterpret_cast<Ask*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONUP:
            if (const POINT p {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; a) {
                if (PtInRect(&a->yes_button, p)) {
                    a->answer = Answer::Yes;
                } else if (PtInRect(&a->no_button, p)) {
                    a->answer = Answer::No;
                }
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT r;
            GetClientRect(hwnd, &r);
            // Drawn aside, then at once: no flicker as it flashes.
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, r.right, r.bottom);
            HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
            const double g = a ? a->glow : 0;
            const auto mix = [g](int from, int to) {
                return static_cast<BYTE>(std::lround(from + (to - from) * g));
            };
            HBRUSH edge = CreateSolidBrush(RGB(220, 38, 38));
            FillRect(memory, &r, edge);
            DeleteObject(edge);
            const int border = std::max(4, static_cast<int>(r.bottom - r.top) / 40);
            RECT inside {r.left + border, r.top + border, r.right - border, r.bottom - border};
            HBRUSH back = CreateSolidBrush(RGB(255, mix(243, 190), mix(176, 90)));
            FillRect(memory, &inside, back);
            DeleteObject(back);
            if (a) {
                SetBkMode(memory, TRANSPARENT);
                HGDIOBJ old_font = SelectObject(memory, a->font);
                SetTextColor(memory, RGB(30, 30, 30));
                RECT text {inside.left + 12, inside.top + 8, inside.right - 12, a->yes_button.top - 24};
                DrawTextW(memory, a->text.c_str(), -1, &text, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
                SelectObject(memory, a->small_font);
                SetTextColor(memory, RGB(90, 60, 20));
                RECT left {inside.left, a->yes_button.top - 24, inside.right, a->yes_button.top - 4};
                DrawTextW(memory, a->left.c_str(), -1, &left, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(memory, old_font);
                draw_button(memory, a->yes_button, RGB(22, 163, 74), RGB(10, 100, 40), a->yes, a->button_font);
                draw_button(memory, a->no_button, RGB(220, 38, 38), RGB(150, 20, 20), a->no, a->button_font);
            }
            BitBlt(dc, 0, 0, r.right, r.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, old_bitmap);
            DeleteObject(bitmap);
            DeleteDC(memory);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    const wchar_t* ask_class() {
        static const ATOM atom = [] {
            WNDCLASSW wc {};
            wc.lpfnWndProc = ask_proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_HAND);
            wc.lpszClassName = L"zchat_ask";
            return RegisterClassW(&wc);
        }();
        (void)atom;
        return L"zchat_ask";
    }

    HFONT make_font(double height, int weight) {
        return CreateFontW(-px(height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }

    // The picture at w x h, smoothly, as a layered window wants it: BGRA, the colors premultiplied by their alpha.
    void scale_into(std::uint32_t* out, int w, int h, const image::DecodedImage& picture) {
        const unsigned char* src = picture.pixels.get();
        const int sw = picture.width;
        const int sh = picture.height;
        const auto at = [&](int x, int y, int k) {
            return static_cast<double>(src[(static_cast<std::size_t>(y) * sw + x) * 4 + k]);
        };
        for (int y = 0; y < h; ++y) {
            const double fy = std::clamp((y + 0.5) * sh / h - 0.5, 0.0, sh - 1.0);
            const int y0 = static_cast<int>(fy);
            const int y1 = std::min(y0 + 1, sh - 1);
            const double ty = fy - y0;
            for (int x = 0; x < w; ++x) {
                const double fx = std::clamp((x + 0.5) * sw / w - 0.5, 0.0, sw - 1.0);
                const int x0 = static_cast<int>(fx);
                const int x1 = std::min(x0 + 1, sw - 1);
                const double tx = fx - x0;
                double c[4];
                for (int k = 0; k < 4; ++k) {
                    c[k] = (at(x0, y0, k) * (1 - tx) + at(x1, y0, k) * tx) * (1 - ty) +
                           (at(x0, y1, k) * (1 - tx) + at(x1, y1, k) * tx) * ty;
                }
                const double alpha = c[3] / 255;
                out[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint32_t>(std::lround(c[3])) << 24 |
                                                           static_cast<std::uint32_t>(std::lround(c[0] * alpha)) << 16 |
                                                           static_cast<std::uint32_t>(std::lround(c[1] * alpha)) << 8 |
                                                           static_cast<std::uint32_t>(std::lround(c[2] * alpha));
            }
        }
    }

} // namespace

bool available() {
    return true;
}

void set_dispatcher(std::function<void(std::function<void()>)>) {
    // Not needed: the popups are made on the thread that asks.
}

bool alert(std::string_view text, std::chrono::milliseconds wait, std::stop_token stop) {
    const double s = dpi_scale();
    const RECT area = work_area();
    const int w = px(300 * s);
    const int h = px(130 * s);
    Alert a;
    a.text = widen(text);
    a.stop = {px(24 * s), px(50 * s), w - px(24 * s), h - px(16 * s)};
    a.font = make_font(17 * s, FW_SEMIBOLD);
    a.button_font = make_font(26 * s, FW_BOLD);
    Mover m;
    place(m, w, h, area.left, area.top, area.right, area.bottom);
    aim(m, alert_speed * s);
    const HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, alert_class(),
                                      L"zchat trill", WS_POPUP | WS_BORDER, px(m.x), px(m.y), w, h, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
    bool pressed = false;
    if (hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&a));
        const auto end = clock::now() + wait;
        auto last = clock::now();
        long long shown = -1;
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        while (!a.pressed && !stop.stop_requested()) {
            const auto now = clock::now();
            if (now >= end) {
                break;
            }
            if (const auto left = std::chrono::ceil<std::chrono::seconds>(end - now).count(); left != shown) {
                shown = left;
                a.button = std::format(L"STOP ({})", left);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            // Now and then another way, so where it goes cannot be guessed.
            if (random(0, 1) < 0.04) {
                aim(m, alert_speed * s);
            }
            m.step(seconds(now - last), w, h, area.left, area.top, area.right, area.bottom);
            last = now;
            SetWindowPos(hwnd, HWND_TOPMOST, px(m.x), px(m.y), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            pump(15);
        }
        pressed = a.pressed;
        DestroyWindow(hwnd);
    }
    DeleteObject(a.font);
    DeleteObject(a.button_font);
    return pressed;
}

Answer ask(std::string_view text, std::string_view yes, std::string_view no, std::chrono::milliseconds wait,
           std::stop_token stop) {
    const double s = dpi_scale();
    const RECT area = work_area();
    const int w = std::min(px(400 * s), static_cast<int>(area.right - area.left));
    const int h = std::min(px(170 * s), static_cast<int>(area.bottom - area.top));
    const int x = area.left + (area.right - area.left - w) / 2;
    const int y = area.top + (area.bottom - area.top - h) / 2;
    Ask a;
    a.text = widen(text);
    a.yes = widen(yes);
    a.no = widen(no);
    const int button_w = (w - px(52 * s)) / 2;
    const int button_top = h - px(58 * s);
    const int button_bottom = h - px(14 * s);
    a.yes_button = {px(18 * s), button_top, px(18 * s) + button_w, button_bottom};
    a.no_button = {w - px(18 * s) - button_w, button_top, w - px(18 * s), button_bottom};
    a.font = make_font(16 * s, FW_SEMIBOLD);
    a.small_font = make_font(12 * s, FW_NORMAL);
    a.button_font = make_font(18 * s, FW_BOLD);
    // In the taskbar too, flashing there.
    const HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW | WS_EX_NOACTIVATE, ask_class(), L"zchat",
                                      WS_POPUP, x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Answer answer = Answer::None;
    if (hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&a));
        const auto start = clock::now();
        const auto end = start + wait;
        long long shown = -1;
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        FLASHWINFO flash {sizeof flash, hwnd, FLASHW_TRAY | FLASHW_TIMER, 0, 0};
        FlashWindowEx(&flash);
        const double shake = 8 * s;
        while (a.answer == Answer::None && !stop.stop_requested()) {
            const auto now = clock::now();
            if (now >= end) {
                break;
            }
            if (const auto left = std::chrono::ceil<std::chrono::seconds>(end - now).count(); left != shown) {
                shown = left;
                a.left = std::format(L"{} seconds left to answer", left);
            }
            const double t = seconds(now - start);
            a.glow = (1 + std::sin(t * 2 * std::numbers::pi * 1.5)) / 2;
            InvalidateRect(hwnd, nullptr, FALSE);
            // Shaking the first second, and again every five; always above the others (which may want to be too).
            const double since = std::fmod(t, 5.0);
            const double amplitude = since < 1 ? shake * (1 - since) : 0;
            SetWindowPos(hwnd, HWND_TOPMOST, x + px(random(-amplitude, amplitude)), y + px(random(-amplitude, amplitude)),
                         0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            pump(15);
        }
        answer = a.answer;
        DestroyWindow(hwnd);
    }
    DeleteObject(a.font);
    DeleteObject(a.small_font);
    DeleteObject(a.button_font);
    return answer;
}

void fly(const image::DecodedImage& picture, std::chrono::milliseconds duration, std::stop_token stop) {
    if (!picture.pixels || picture.width <= 0 || picture.height <= 0) {
        return;
    }
    const RECT area = work_area();
    const double aw = area.right - area.left;
    const double ah = area.bottom - area.top;
    // Big, whatever its own size.
    const double fit = std::min(aw, ah) * picture_share / std::max(picture.width, picture.height);
    const int w = std::max(1, px(picture.width * fit));
    const int h = std::max(1, px(picture.height * fit));
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    // Negative: top-down, as the pixels are.
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) {
        scale_into(static_cast<std::uint32_t*>(bits), w, h, picture);
        const HGDIOBJ old = SelectObject(memory, bitmap);
        // Layered, to be drawn with its transparency; and clicks go through it.
        const HWND hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, picture_class(),
            L"zchat trill", WS_POPUP, 0, 0, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (hwnd) {
            const double shake = shake_pixels * dpi_scale();
            Mover m;
            place(m, w, h, area.left, area.top, area.right, area.bottom);
            // Across the screen in about two seconds and a half.
            aim(m, std::hypot(aw, ah) / 2.5);
            const auto start = clock::now();
            auto last = start;
            const double total = seconds(duration);
            bool shown = false;
            while (!stop.stop_requested()) {
                const auto now = clock::now();
                const double t = seconds(now - start);
                if (t >= total) {
                    break;
                }
                m.step(seconds(now - last), w, h, area.left, area.top, area.right, area.bottom);
                last = now;
                // Shaking all the way, faded in and out.
                POINT to {px(m.x + random(-shake, shake)), px(m.y + random(-shake, shake))};
                SIZE size {w, h};
                POINT from {0, 0};
                const double fade = std::clamp(std::min(t / 0.15, (total - t) / 0.3), 0.0, 1.0);
                BLENDFUNCTION blend {AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(255 * fade)), AC_SRC_ALPHA};
                UpdateLayeredWindow(hwnd, screen, &to, &size, memory, &from, 0, &blend, ULW_ALPHA);
                if (!shown) {
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    shown = true;
                }
                pump(15);
            }
            DestroyWindow(hwnd);
        }
        SelectObject(memory, old);
    }
    if (bitmap) {
        DeleteObject(bitmap);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

void flash(void* window) {
    const auto hwnd = static_cast<HWND>(window);
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    // In the taskbar, until the window is looked at again.
    FLASHWINFO info {sizeof info, hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
    FlashWindowEx(&info);
}

void nudge(void* window) {
    using namespace std::chrono_literals;
    const auto hwnd = static_cast<HWND>(window);
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    if (IsIconic(hwnd)) {
        ShowWindowAsync(hwnd, SW_RESTORE);
        for (int i = 0; i < 25 && IsIconic(hwnd); ++i) {
            std::this_thread::sleep_for(20ms);
        }
    }
    // To the front: a program in the background may not take the focus, but its window can go above the others.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(hwnd);
    FLASHWINFO flash {sizeof flash, hwnd, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
    FlashWindowEx(&flash);
    // A maximized window stays where it is.
    RECT r;
    if (IsZoomed(hwnd) || !GetWindowRect(hwnd, &r)) {
        return;
    }
    const double s = dpi_scale();
    constexpr int shakes = 20;
    for (int i = 0; i < shakes; ++i) {
        const int amplitude = px(12 * s * (shakes - i) / shakes);
        const int dx = i % 2 ? amplitude : -amplitude;
        const int dy = (i / 2) % 2 ? amplitude / 2 : -amplitude / 2;
        SetWindowPos(hwnd, nullptr, r.left + dx, r.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        std::this_thread::sleep_for(25ms);
    }
    SetWindowPos(hwnd, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

namespace {

    COLORREF rgb(std::uint32_t color) {
        return RGB(color >> 16 & 0xFF, color >> 8 & 0xFF, color & 0xFF);
    }

    constexpr std::uint32_t toast_back = 0x1e222a;

    // A notification shown, with what its window procedure draws.
    struct WinToast {
        Toast toast;
        std::wstring where;
        std::wstring title;
        std::wstring text;
        std::wstring letter;
        HWND hwnd = nullptr;
        ToastLife life;
        double s = 1;
        int w = 0;
        int h = 0;
        // Where it is now; x is -1 until it is first shown.
        int x = -1;
        int y = 0;
        BYTE alpha = 0;
        RECT close {};
        bool close_hot = false;
        // Its circle, d x d, on its back: a picture of 32 bits a pixel.
        HBITMAP avatar = nullptr;
        int d = 0;
        HFONT small_font = nullptr;
        HFONT title_font = nullptr;
        HFONT text_font = nullptr;
        HFONT letter_font = nullptr;

        ~WinToast() {
            if (hwnd) {
                DestroyWindow(hwnd);
            }
            for (HGDIOBJ o : {static_cast<HGDIOBJ>(avatar), static_cast<HGDIOBJ>(small_font),
                              static_cast<HGDIOBJ>(title_font), static_cast<HGDIOBJ>(text_font),
                              static_cast<HGDIOBJ>(letter_font)}) {
                if (o) {
                    DeleteObject(o);
                }
            }
        }
    };

    // The notifications shown, the oldest first, and what moves them; on the zchat window's thread only.
    std::vector<std::unique_ptr<WinToast>> win_toasts;
    UINT_PTR toast_timer = 0;
    clock::time_point toast_last;

    void paint_toast(HDC dc, const WinToast& t) {
        const RECT r {0, 0, t.w, t.h};
        const double s = t.s;
        HBRUSH back = CreateSolidBrush(rgb(toast_back));
        FillRect(dc, &r, back);
        DeleteObject(back);
        HBRUSH accent = CreateSolidBrush(rgb(t.toast.accent));
        const RECT stripe {0, 0, px(4 * s), t.h};
        FillRect(dc, &stripe, accent);
        DeleteObject(accent);
        const int ax = px(18 * s);
        const int ay = px(16 * s);
        if (t.avatar) {
            HDC picture = CreateCompatibleDC(dc);
            const HGDIOBJ old = SelectObject(picture, t.avatar);
            BitBlt(dc, ax, ay, t.d, t.d, picture, 0, 0, SRCCOPY);
            SelectObject(picture, old);
            DeleteDC(picture);
        }
        SetBkMode(dc, TRANSPARENT);
        const HGDIOBJ old_font = SelectObject(dc, t.letter_font);
        if (!t.toast.picture) {
            SetTextColor(dc, RGB(255, 255, 255));
            RECT letter {ax, ay, ax + t.d, ay + t.d};
            DrawTextW(dc, t.letter.c_str(), -1, &letter, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        const int left = ax + t.d + px(12 * s);
        const int right = t.close.left - px(4 * s);
        SelectObject(dc, t.small_font);
        SetTextColor(dc, RGB(0x8b, 0x92, 0x9f));
        RECT where {left, px(12 * s), right, px(28 * s)};
        DrawTextW(dc, t.where.c_str(), -1, &where, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, t.title_font);
        SetTextColor(dc, RGB(0xf0, 0xf2, 0xf5));
        RECT title {left, px(29 * s), right, px(49 * s)};
        DrawTextW(dc, t.title.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, t.text_font);
        SetTextColor(dc, RGB(0xc4, 0xc9, 0xd2));
        RECT text {left, px(51 * s), t.w - px(14 * s), t.h - px(8 * s)};
        DrawTextW(dc, t.text.c_str(), -1, &text,
                  DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX);
        // The ×, lit while the mouse is on it.
        if (t.close_hot) {
            HBRUSH hot = CreateSolidBrush(RGB(0x34, 0x3a, 0x46));
            HPEN none = CreatePen(PS_NULL, 0, 0);
            const HGDIOBJ old_brush = SelectObject(dc, hot);
            const HGDIOBJ old_pen = SelectObject(dc, none);
            RoundRect(dc, t.close.left, t.close.top, t.close.right, t.close.bottom, px(6 * s), px(6 * s));
            SelectObject(dc, old_pen);
            SelectObject(dc, old_brush);
            DeleteObject(none);
            DeleteObject(hot);
        }
        SelectObject(dc, t.title_font);
        SetTextColor(dc, t.close_hot ? RGB(0xf0, 0xf2, 0xf5) : RGB(0x8b, 0x92, 0x9f));
        RECT close = t.close;
        DrawTextW(dc, L"×", -1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old_font);
    }

    LRESULT CALLBACK toast_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* t = reinterpret_cast<WinToast*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_MOUSEACTIVATE:
            // Clicked without taking the focus from what the user types in.
            return MA_NOACTIVATE;
        case WM_LBUTTONUP:
            if (const POINT p {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; t && t->life.going < 0) {
                t->life.close();
                // Not the ×: what it is about.
                if (!PtInRect(&t->close, p) && t->toast.clicked) {
                    // A copy: what it does may show another notification, and move this one.
                    const auto clicked = t->toast.clicked;
                    clicked();
                }
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (t) {
                // Drawn aside, then at once: no flicker.
                HDC memory = CreateCompatibleDC(dc);
                HBITMAP bitmap = CreateCompatibleBitmap(dc, t->w, t->h);
                const HGDIOBJ old = SelectObject(memory, bitmap);
                paint_toast(memory, *t);
                BitBlt(dc, 0, 0, t->w, t->h, memory, 0, 0, SRCCOPY);
                SelectObject(memory, old);
                DeleteObject(bitmap);
                DeleteDC(memory);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    const wchar_t* toast_class() {
        static const ATOM atom = [] {
            WNDCLASSW wc {};
            wc.style = CS_DROPSHADOW;
            wc.lpfnWndProc = toast_proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_HAND);
            wc.lpszClassName = L"zchat_toast";
            return RegisterClassW(&wc);
        }();
        (void)atom;
        return L"zchat_toast";
    }

    // Rounded corners: Windows 11 draws them itself; before it, the window is cut round.
    void round_corners(HWND hwnd, int w, int h, double s) {
        using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        static const auto set_attribute = [] {
            const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
            return dwm ? reinterpret_cast<SetAttribute>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")))
                       : nullptr;
        }();
        // DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUND.
        constexpr DWORD corner_preference = 33;
        const DWORD round = 2;
        if (!set_attribute || FAILED(set_attribute(hwnd, corner_preference, &round, sizeof round))) {
            SetWindowRgn(hwnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, px(14 * s), px(14 * s)), FALSE);
        }
    }

    // A frame of all the notifications: they come, go, and move to their places.
    void CALLBACK step_toasts(HWND, UINT, UINT_PTR, DWORD) {
        const auto now = clock::now();
        const double dt = std::clamp(seconds(now - toast_last), 0.0, 0.1);
        toast_last = now;
        std::erase_if(win_toasts, [](const auto& t) {
            return t->life.gone();
        });
        // Too many: the oldest go.
        auto staying = std::ranges::count_if(win_toasts, [](const auto& t) {
            return t->life.going < 0;
        });
        for (auto& t : win_toasts) {
            if (staying <= static_cast<std::ptrdiff_t>(max_toasts)) {
                break;
            }
            if (t->life.going < 0) {
                t->life.close();
                --staying;
            }
        }
        if (win_toasts.empty()) {
            KillTimer(nullptr, toast_timer);
            toast_timer = 0;
            return;
        }
        // The work area of the main screen, where the taskbar's notifications are too.
        RECT area {};
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0)) {
            area = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        }
        const double s = win_toasts.back()->s;
        const auto tops = toast_places(win_toasts.size(), win_toasts.back()->h, toast_gap * s, toast_margin * s,
                                       area.bottom);
        POINT mouse {};
        GetCursorPos(&mouse);
        for (std::size_t i = 0; i < win_toasts.size(); ++i) {
            WinToast& t = *win_toasts[i];
            const RECT on {t.x, t.y, t.x + t.w, t.y + t.h};
            t.life.hovered = t.x >= 0 && PtInRect(&on, mouse);
            const POINT inside {mouse.x - t.x, mouse.y - t.y};
            if (const bool hot = t.life.hovered && PtInRect(&t.close, inside); hot != t.close_hot) {
                t.close_hot = hot;
                InvalidateRect(t.hwnd, nullptr, FALSE);
            }
            t.life.step(dt, tops[i]);
            const int x = area.right - px(toast_margin * s) - t.w + px(t.life.slide() * 48 * s);
            const int y = px(t.life.y);
            const auto alpha = static_cast<BYTE>(std::lround(255 * t.life.opacity()));
            if (alpha != t.alpha) {
                t.alpha = alpha;
                SetLayeredWindowAttributes(t.hwnd, 0, alpha, LWA_ALPHA);
            }
            if (x != t.x || y != t.y) {
                const bool first = t.x < 0;
                t.x = x;
                t.y = y;
                SetWindowPos(t.hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                if (first) {
                    ShowWindow(t.hwnd, SW_SHOWNOACTIVATE);
                }
            }
        }
    }

} // namespace

bool in_front(void* window) {
    const auto hwnd = static_cast<HWND>(window);
    return hwnd && GetForegroundWindow() == hwnd && !IsIconic(hwnd);
}

void raise(void* window) {
    const auto hwnd = static_cast<HWND>(window);
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    // Allowed: the click on the notification was the last thing the user did, and it was in zchat.
    SetForegroundWindow(hwnd);
}

void toast(Toast toast) {
    auto t = std::make_unique<WinToast>();
    const double s = dpi_scale();
    t->s = s;
    t->w = px(toast_width * s);
    t->h = px(toast_height * s);
    t->where = widen(toast.where);
    t->title = widen(toast.title);
    t->text = widen(toast.text);
    t->letter = widen(initial(toast.title));
    t->close = {t->w - px((10 + toast_close) * s), px(8 * s), t->w - px(10 * s), px((8 + toast_close) * s)};
    t->small_font = make_font(12 * s, FW_NORMAL);
    t->title_font = make_font(15 * s, FW_SEMIBOLD);
    t->text_font = make_font(14 * s, FW_NORMAL);
    t->letter_font = make_font(19 * s, FW_BOLD);
    // The circle: the picture, or the color with the initial over it.
    t->d = px(toast_avatar * s);
    const image::DecodedImage solid = solid_image(toast.accent);
    const image::DecodedImage* circle = toast.picture ? toast.picture.get() : &solid;
    if (circle->pixels) {
        const auto pixels = circle_picture(*circle, t->d, toast_back);
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = t->d;
        info.bmiHeader.biHeight = -t->d;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        t->avatar = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (t->avatar && bits) {
            auto* out = static_cast<std::uint32_t*>(bits);
            for (std::size_t i = 0; i < pixels.size() / 3; ++i) {
                out[i] = static_cast<std::uint32_t>(pixels[i * 3]) << 16 |
                         static_cast<std::uint32_t>(pixels[i * 3 + 1]) << 8 | pixels[i * 3 + 2];
            }
        }
    }
    t->toast = std::move(toast);
    t->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, toast_class(),
                              L"zchat", WS_POPUP, 0, 0, t->w, t->h, nullptr, nullptr, GetModuleHandleW(nullptr),
                              nullptr);
    if (!t->hwnd) {
        return;
    }
    SetWindowLongPtrW(t->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(t.get()));
    SetLayeredWindowAttributes(t->hwnd, 0, 0, LWA_ALPHA);
    round_corners(t->hwnd, t->w, t->h, s);
    win_toasts.push_back(std::move(t));
    if (!toast_timer) {
        toast_last = clock::now();
        // A timer of the thread: its loop (the window's) runs step_toasts().
        toast_timer = SetTimer(nullptr, 0, 15, step_toasts);
    }
}

#elif defined(ZCHAT_POPUPS_GTK)
// Not tested yet: written without a Linux machine at hand.
namespace {

    std::mutex dispatch_mutex;
    std::function<void(std::function<void()>)> dispatcher;

    // Runs work on GTK's thread; false when there is no window (anymore).
    bool on_gtk(std::function<void()> work) {
        std::scoped_lock lock(dispatch_mutex);
        if (!dispatcher) {
            return false;
        }
        dispatcher(std::move(work));
        return true;
    }

    // How a popup made on GTK's thread tells the thread waiting for it that it is over, and how that one tells it to
    // end early.
    struct Done {
        std::mutex mutex;
        std::condition_variable_any cv;
        bool over = false;
        bool pressed = false;
        Answer answer = Answer::None;
        std::atomic<bool> cancel {false};

        void finish(bool stop_pressed, Answer given = Answer::None) {
            {
                std::scoped_lock lock(mutex);
                over = true;
                pressed = stop_pressed;
                answer = given;
            }
            cv.notify_all();
        }

        // Waits until it is over, or until stop is requested (then it is told to end). Returns whether STOP was
        // pressed.
        bool wait(std::stop_token stop) {
            std::unique_lock lock(mutex);
            if (!cv.wait(lock, stop, [this] {
                    return over;
                })) {
                cancel = true;
            }
            return pressed;
        }

        // The same, for a question: returns its answer.
        Answer wait_answer(std::stop_token stop) {
            std::unique_lock lock(mutex);
            if (!cv.wait(lock, stop, [this] {
                    return over;
                })) {
                cancel = true;
            }
            return answer;
        }
    };

    // The work area of the main screen.
    GdkRectangle work_area() {
        GdkRectangle area {0, 0, 1280, 720};
        GdkDisplay* display = gdk_display_get_default();
        GdkMonitor* monitor = display ? gdk_display_get_primary_monitor(display) : nullptr;
        if (!monitor && display) {
            monitor = gdk_display_get_monitor(display, 0);
        }
        if (monitor) {
            gdk_monitor_get_workarea(monitor, &area);
        }
        return area;
    }

    // A window above the others, without a title bar or a place in the taskbar, that does not take the focus, and
    // that only zchat closes.
    GtkWidget* popup_window() {
        GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
        gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
        gtk_window_set_accept_focus(GTK_WINDOW(window), FALSE);
        gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_UTILITY);
        g_signal_connect(window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer) -> gboolean {
                             return TRUE;
                         }),
                         nullptr);
        return window;
    }

    struct GtkAlert {
        std::shared_ptr<Done> done;
        GtkWidget* window = nullptr;
        GtkWidget* button = nullptr;
        GdkRectangle area {};
        int width = 300;
        int height = 130;
        gint64 end = 0;
        gint64 last = 0;
        gint64 shown = -1;
        Mover mover;
        bool pressed = false;
    };

    gboolean alert_step(gpointer data) {
        auto* a = static_cast<GtkAlert*>(data);
        const gint64 now = g_get_monotonic_time();
        if (a->pressed || a->done->cancel || now >= a->end) {
            gtk_widget_destroy(a->window);
            a->done->finish(a->pressed);
            delete a;
            return G_SOURCE_REMOVE;
        }
        if (const gint64 left = (a->end - now + 999'999) / 1'000'000; left != a->shown) {
            a->shown = left;
            gtk_button_set_label(GTK_BUTTON(a->button), std::format("STOP ({})", left).c_str());
        }
        if (random(0, 1) < 0.04) {
            aim(a->mover, alert_speed);
        }
        a->mover.step(static_cast<double>(now - a->last) / 1e6, a->width, a->height, a->area.x, a->area.y,
                      a->area.x + a->area.width, a->area.y + a->area.height);
        a->last = now;
        gtk_window_move(GTK_WINDOW(a->window), static_cast<gint>(std::lround(a->mover.x)),
                        static_cast<gint>(std::lround(a->mover.y)));
        return G_SOURCE_CONTINUE;
    }

    struct GtkAsk {
        std::shared_ptr<Done> done;
        GtkWidget* window = nullptr;
        GtkWidget* left = nullptr;
        gint x = 0;
        gint y = 0;
        gint64 start = 0;
        gint64 end = 0;
        gint64 shown = -1;
        Answer answer = Answer::None;
    };

    gboolean ask_step(gpointer data) {
        auto* a = static_cast<GtkAsk*>(data);
        const gint64 now = g_get_monotonic_time();
        if (a->answer != Answer::None || a->done->cancel || now >= a->end) {
            gtk_widget_destroy(a->window);
            a->done->finish(false, a->answer);
            delete a;
            return G_SOURCE_REMOVE;
        }
        if (const gint64 left = (a->end - now + 999'999) / 1'000'000; left != a->shown) {
            a->shown = left;
            gtk_label_set_text(GTK_LABEL(a->left), std::format("{} seconds left to answer", left).c_str());
            // Above the others again, now and then.
            gtk_window_set_keep_above(GTK_WINDOW(a->window), TRUE);
            gtk_window_set_urgency_hint(GTK_WINDOW(a->window), TRUE);
        }
        // Shaking the first second, and again every five.
        const double since = std::fmod(static_cast<double>(now - a->start) / 1e6, 5.0);
        const double amplitude = since < 1 ? shake_pixels * (1 - since) : 0;
        gtk_window_move(GTK_WINDOW(a->window), a->x + static_cast<gint>(std::lround(random(-amplitude, amplitude))),
                        a->y + static_cast<gint>(std::lround(random(-amplitude, amplitude))));
        return G_SOURCE_CONTINUE;
    }

    struct GtkFly {
        std::shared_ptr<Done> done;
        GtkWidget* window = nullptr;
        GdkRectangle area {};
        int width = 0;
        int height = 0;
        gint64 end = 0;
        gint64 last = 0;
        Mover mover;
    };

    gboolean fly_step(gpointer data) {
        auto* f = static_cast<GtkFly*>(data);
        const gint64 now = g_get_monotonic_time();
        if (f->done->cancel || now >= f->end) {
            gtk_widget_destroy(f->window);
            f->done->finish(false);
            delete f;
            return G_SOURCE_REMOVE;
        }
        f->mover.step(static_cast<double>(now - f->last) / 1e6, f->width, f->height, f->area.x, f->area.y,
                      f->area.x + f->area.width, f->area.y + f->area.height);
        f->last = now;
        gtk_window_move(GTK_WINDOW(f->window),
                        static_cast<gint>(std::lround(f->mover.x + random(-shake_pixels, shake_pixels))),
                        static_cast<gint>(std::lround(f->mover.y + random(-shake_pixels, shake_pixels))));
        return G_SOURCE_CONTINUE;
    }

    struct GtkNudge {
        GtkWindow* window = nullptr;
        gint x = 0;
        gint y = 0;
        int step = 0;
    };

    gboolean nudge_step(gpointer data) {
        constexpr int shakes = 20;
        auto* n = static_cast<GtkNudge*>(data);
        if (n->step >= shakes) {
            gtk_window_move(n->window, n->x, n->y);
            g_object_unref(n->window);
            delete n;
            return G_SOURCE_REMOVE;
        }
        const int amplitude = 12 * (shakes - n->step) / shakes;
        gtk_window_move(n->window, n->x + (n->step % 2 ? amplitude : -amplitude),
                        n->y + ((n->step / 2) % 2 ? amplitude / 2 : -amplitude / 2));
        ++n->step;
        return G_SOURCE_CONTINUE;
    }

} // namespace

bool available() {
    std::scoped_lock lock(dispatch_mutex);
    return static_cast<bool>(dispatcher);
}

void set_dispatcher(std::function<void(std::function<void()>)> dispatch) {
    std::scoped_lock lock(dispatch_mutex);
    dispatcher = std::move(dispatch);
}

bool alert(std::string_view text, std::chrono::milliseconds wait, std::stop_token stop) {
    auto done = std::make_shared<Done>();
    if (!on_gtk([done, text = std::string(text), wait] {
            auto* a = new GtkAlert;
            a->done = done;
            a->area = work_area();
            a->window = popup_window();
            gtk_window_set_default_size(GTK_WINDOW(a->window), a->width, a->height);
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
            gtk_container_set_border_width(GTK_CONTAINER(box), 12);
            gtk_box_pack_start(GTK_BOX(box), gtk_label_new(text.c_str()), FALSE, FALSE, 0);
            a->button = gtk_button_new_with_label("STOP");
            gtk_box_pack_start(GTK_BOX(box), a->button, TRUE, TRUE, 0);
            gtk_container_add(GTK_CONTAINER(a->window), box);
            // Pressed, not clicked: it runs away before the button is let go.
            g_signal_connect(a->button, "button-press-event",
                             G_CALLBACK(+[](GtkWidget*, GdkEventButton*, gpointer alert) -> gboolean {
                                 static_cast<GtkAlert*>(alert)->pressed = true;
                                 return TRUE;
                             }),
                             a);
            place(a->mover, a->width, a->height, a->area.x, a->area.y, a->area.x + a->area.width,
                  a->area.y + a->area.height);
            aim(a->mover, alert_speed);
            gtk_window_move(GTK_WINDOW(a->window), static_cast<gint>(a->mover.x), static_cast<gint>(a->mover.y));
            gtk_widget_show_all(a->window);
            a->last = g_get_monotonic_time();
            a->end = a->last + static_cast<gint64>(wait.count()) * 1000;
            g_timeout_add(16, alert_step, a);
        })) {
        return false;
    }
    return done->wait(stop);
}

Answer ask(std::string_view text, std::string_view yes, std::string_view no, std::chrono::milliseconds wait,
           std::stop_token stop) {
    auto done = std::make_shared<Done>();
    if (!on_gtk([done, text = std::string(text), yes = std::string(yes), no = std::string(no), wait] {
            auto* a = new GtkAsk;
            a->done = done;
            a->window = popup_window();
            gtk_window_set_default_size(GTK_WINDOW(a->window), 400, 170);
            GtkWidget* frame = gtk_event_box_new();
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
            gtk_container_set_border_width(GTK_CONTAINER(box), 14);
            GtkWidget* label = gtk_label_new(nullptr);
            gchar* markup = g_markup_printf_escaped("<span size='x-large' weight='bold'>%s</span>", text.c_str());
            gtk_label_set_markup(GTK_LABEL(label), markup);
            g_free(markup);
            gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
            gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
            gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
            a->left = gtk_label_new("");
            gtk_box_pack_start(GTK_BOX(box), a->left, FALSE, FALSE, 0);
            GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
            gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
            GtkWidget* yes_button = gtk_button_new_with_label(yes.c_str());
            GtkWidget* no_button = gtk_button_new_with_label(no.c_str());
            gtk_widget_set_size_request(yes_button, -1, 42);
            gtk_box_pack_start(GTK_BOX(buttons), yes_button, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(buttons), no_button, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(frame), box);
            gtk_container_add(GTK_CONTAINER(a->window), frame);
            GtkCssProvider* css = gtk_css_provider_new();
            gtk_css_provider_load_from_data(css, "* { background-color: #fff3b0; color: #1e1e1e; }"
                                                 "window { border: 4px solid #dc2626; }", -1, nullptr);
            gtk_style_context_add_provider(gtk_widget_get_style_context(a->window), GTK_STYLE_PROVIDER(css),
                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            gtk_style_context_add_provider(gtk_widget_get_style_context(frame), GTK_STYLE_PROVIDER(css),
                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(css);
            g_signal_connect(yes_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer ask) {
                                 static_cast<GtkAsk*>(ask)->answer = Answer::Yes;
                             }),
                             a);
            g_signal_connect(no_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer ask) {
                                 static_cast<GtkAsk*>(ask)->answer = Answer::No;
                             }),
                             a);
            const GdkRectangle area = work_area();
            a->x = area.x + std::max(0, (area.width - 400) / 2);
            a->y = area.y + std::max(0, (area.height - 170) / 2);
            gtk_window_move(GTK_WINDOW(a->window), a->x, a->y);
            gtk_widget_show_all(a->window);
            a->start = g_get_monotonic_time();
            a->end = a->start + static_cast<gint64>(wait.count()) * 1000;
            g_timeout_add(16, ask_step, a);
        })) {
        return Answer::None;
    }
    return done->wait_answer(stop);
}

void fly(const image::DecodedImage& picture, std::chrono::milliseconds duration, std::stop_token stop) {
    if (!picture.pixels || picture.width <= 0 || picture.height <= 0) {
        return;
    }
    // A copy: this may return (when stopped) before GTK is done with it.
    const unsigned char* pixels = picture.pixels.get();
    auto copy = std::make_shared<std::vector<unsigned char>>(pixels, pixels + static_cast<std::size_t>(picture.width) *
                                                                                  picture.height * 4);
    auto done = std::make_shared<Done>();
    const int pw = picture.width;
    const int ph = picture.height;
    if (!on_gtk([done, copy, pw, ph, duration] {
            auto* f = new GtkFly;
            f->done = done;
            f->area = work_area();
            const double fit = std::min(f->area.width, f->area.height) * picture_share / std::max(pw, ph);
            f->width = std::max(1, static_cast<int>(std::lround(pw * fit)));
            f->height = std::max(1, static_cast<int>(std::lround(ph * fit)));
            GdkPixbuf* source =
                gdk_pixbuf_new_from_data(copy->data(), GDK_COLORSPACE_RGB, TRUE, 8, pw, ph, pw * 4, nullptr, nullptr);
            GdkPixbuf* scaled = gdk_pixbuf_scale_simple(source, f->width, f->height, GDK_INTERP_BILINEAR);
            g_object_unref(source);
            f->window = popup_window();
            gtk_container_add(GTK_CONTAINER(f->window), gtk_image_new_from_pixbuf(scaled));
            g_object_unref(scaled);
            place(f->mover, f->width, f->height, f->area.x, f->area.y, f->area.x + f->area.width,
                  f->area.y + f->area.height);
            aim(f->mover, std::hypot(f->area.width, f->area.height) / 2.5);
            gtk_window_move(GTK_WINDOW(f->window), static_cast<gint>(f->mover.x), static_cast<gint>(f->mover.y));
            gtk_widget_show_all(f->window);
            // Clicks go through it.
            cairo_region_t* none = cairo_region_create();
            gtk_widget_input_shape_combine_region(f->window, none);
            cairo_region_destroy(none);
            f->last = g_get_monotonic_time();
            f->end = f->last + static_cast<gint64>(duration.count()) * 1000;
            g_timeout_add(16, fly_step, f);
        })) {
        return;
    }
    done->wait(stop);
}

void flash(void* window) {
    if (!window) {
        return;
    }
    // Until the window is looked at again: GTK takes the hint away once it has the focus.
    on_gtk([window] {
        if (!gtk_window_is_active(GTK_WINDOW(window))) {
            gtk_window_set_urgency_hint(GTK_WINDOW(window), TRUE);
        }
    });
}

void nudge(void* window) {
    if (!window) {
        return;
    }
    on_gtk([window] {
        GtkWindow* w = GTK_WINDOW(window);
        gtk_window_deiconify(w);
        gtk_window_present(w);
        if (gtk_window_is_maximized(w)) {
            return;
        }
        auto* n = new GtkNudge {w};
        gtk_window_get_position(w, &n->x, &n->y);
        g_object_ref(w);
        g_timeout_add(25, nudge_step, n);
    });
}

namespace {

    constexpr std::uint32_t toast_back = 0x1e222a;

    // A notification shown.
    struct GtkToast {
        Toast toast;
        GtkWidget* window = nullptr;
        ToastLife life;
        // Its circle, RGB on its back, toast_avatar pixels wide times the scale of the screen (made when first drawn).
        std::vector<unsigned char> circle;
        int circle_scale = 0;
        // Whether its corners can be see-through (round).
        bool round = false;
        bool close_hot = false;
        // Where it is now; x is -1 until it is first shown.
        gint x = -1;
        gint y = 0;

        ~GtkToast() {
            if (window) {
                gtk_widget_destroy(window);
            }
        }
    };

    // The notifications shown, the oldest first, and what moves them; on GTK's thread only.
    std::vector<std::unique_ptr<GtkToast>> gtk_toasts;
    guint toast_source = 0;
    gint64 toast_last = 0;

    // Where notifications go: the screen of zchat's window, unless it is Wayland's, where a window cannot choose
    // where it goes: then that of Xwayland, opened besides (on the same screen, through another door). Null when
    // there is none.
    GdkScreen* toast_screen() {
        GdkDisplay* display = gdk_display_get_default();
        if (!display) {
            return nullptr;
        }
        const auto is_x11 = [](GdkDisplay* d) {
            return std::string_view(G_OBJECT_TYPE_NAME(d)) == "GdkX11Display";
        };
        if (is_x11(display)) {
            return gdk_display_get_default_screen(display);
        }
        static GdkDisplay* const xwayland = [&]() -> GdkDisplay* {
            const char* name = std::getenv("DISPLAY");
            GdkDisplay* d = name && *name ? gdk_display_open(name) : nullptr;
            if (d && !is_x11(d)) {
                gdk_display_close(d);
                return nullptr;
            }
            return d;
        }();
        return xwayland ? gdk_display_get_default_screen(xwayland) : nullptr;
    }

    // Without a screen where it can be placed, the desktop's own notification.
    void desktop_notification(const Toast& toast) {
        GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
        if (!bus) {
            return;
        }
        const std::string summary = toast.where.empty() ? toast.title : toast.title + " · " + toast.where;
        // The text may have some markup: plain text must be escaped.
        gchar* body = g_markup_escape_text(toast.text.c_str(), -1);
        g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                               "org.freedesktop.Notifications", "Notify",
                               g_variant_new("(susssasa{sv}i)", "zchat", 0u, "zchat", summary.c_str(), body, nullptr,
                                             nullptr, -1),
                               nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
        g_free(body);
        g_object_unref(bus);
    }

    // A line (or lines) of text at x, y, at most width wide (and lines high), ellipsized.
    void draw_text(cairo_t* cr, GtkWidget* widget, const std::string& text, double size, bool bold,
                   std::uint32_t color, double x, double y, double width, int lines = 1) {
        PangoLayout* layout = gtk_widget_create_pango_layout(widget, text.c_str());
        PangoFontDescription* font = pango_font_description_new();
        pango_font_description_set_family(font, "Sans");
        pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
        pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        pango_layout_set_width(layout, static_cast<int>(width * PANGO_SCALE));
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_height(layout, -lines);
        cairo_set_source_rgb(cr, (color >> 16 & 0xFF) / 255.0, (color >> 8 & 0xFF) / 255.0, (color & 0xFF) / 255.0);
        cairo_move_to(cr, x, y);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    void rounded_rectangle(cairo_t* cr, double x, double y, double w, double h, double r) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + w - r, y + r, r, -std::numbers::pi / 2, 0);
        cairo_arc(cr, x + w - r, y + h - r, r, 0, std::numbers::pi / 2);
        cairo_arc(cr, x + r, y + h - r, r, std::numbers::pi / 2, std::numbers::pi);
        cairo_arc(cr, x + r, y + r, r, std::numbers::pi, 3 * std::numbers::pi / 2);
        cairo_close_path(cr);
    }

    // Where its × is, in the window.
    GdkRectangle toast_close_box() {
        return {static_cast<int>(toast_width - 10 - toast_close), 8, static_cast<int>(toast_close),
                static_cast<int>(toast_close)};
    }

    gboolean draw_toast(GtkWidget* widget, cairo_t* cr, gpointer data) {
        auto& t = *static_cast<GtkToast*>(data);
        const double w = toast_width;
        const double h = toast_height;
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        if (t.round) {
            rounded_rectangle(cr, 0.5, 0.5, w - 1, h - 1, 10);
        } else {
            cairo_rectangle(cr, 0, 0, w, h);
        }
        cairo_set_source_rgb(cr, (toast_back >> 16) / 255.0, (toast_back >> 8 & 0xFF) / 255.0,
                             (toast_back & 0xFF) / 255.0);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0x34 / 255.0, 0x3a / 255.0, 0x46 / 255.0);
        cairo_set_line_width(cr, 1);
        cairo_stroke_preserve(cr);
        cairo_clip(cr);
        const std::uint32_t accent = t.toast.accent;
        cairo_set_source_rgb(cr, (accent >> 16 & 0xFF) / 255.0, (accent >> 8 & 0xFF) / 255.0, (accent & 0xFF) / 255.0);
        cairo_rectangle(cr, 0, 0, 4, h);
        cairo_fill(cr);
        // The circle, in the pixels of the screen: sharp, even where they are smaller than GTK's.
        const int d = static_cast<int>(toast_avatar);
        constexpr double ax = 18;
        constexpr double ay = 16;
        const int scale = std::max(1, gtk_widget_get_scale_factor(widget));
        const int dd = d * scale;
        if (t.circle_scale != scale) {
            t.circle_scale = scale;
            const image::DecodedImage solid = solid_image(t.toast.accent);
            t.circle = circle_picture(t.toast.picture ? *t.toast.picture : solid, dd, toast_back);
        }
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, dd, dd);
        unsigned char* rows = cairo_image_surface_get_data(surface);
        const int stride = cairo_image_surface_get_stride(surface);
        for (int y = 0; y < dd; ++y) {
            auto* row = reinterpret_cast<std::uint32_t*>(rows + static_cast<std::ptrdiff_t>(y) * stride);
            for (int x = 0; x < dd; ++x) {
                const unsigned char* p = &t.circle[(static_cast<std::size_t>(y) * dd + x) * 3];
                row[x] = static_cast<std::uint32_t>(p[0]) << 16 | static_cast<std::uint32_t>(p[1]) << 8 | p[2];
            }
        }
        cairo_surface_mark_dirty(surface);
        cairo_surface_set_device_scale(surface, scale, scale);
        cairo_set_source_surface(cr, surface, ax, ay);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
        cairo_rectangle(cr, ax, ay, d, d);
        cairo_fill(cr);
        cairo_surface_destroy(surface);
        if (!t.toast.picture) {
            PangoLayout* layout = gtk_widget_create_pango_layout(widget, initial(t.toast.title).c_str());
            PangoFontDescription* font = pango_font_description_from_string("Sans Bold");
            pango_font_description_set_absolute_size(font, 18 * PANGO_SCALE);
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);
            int lw = 0;
            int lh = 0;
            pango_layout_get_pixel_size(layout, &lw, &lh);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_move_to(cr, ax + (d - lw) / 2.0, ay + (d - lh) / 2.0);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);
        }
        const GdkRectangle close = toast_close_box();
        const double left = ax + d + 12;
        const double right = close.x - 4;
        draw_text(cr, widget, t.toast.where, 12, false, 0x8b929f, left, 11, right - left);
        draw_text(cr, widget, t.toast.title, 15, true, 0xf0f2f5, left, 28, right - left);
        draw_text(cr, widget, t.toast.text, 14, false, 0xc4c9d2, left, 50, w - 14 - left, 2);
        // The ×, lit while the mouse is on it.
        if (t.close_hot) {
            rounded_rectangle(cr, close.x, close.y, close.width, close.height, 4);
            cairo_set_source_rgb(cr, 0x34 / 255.0, 0x3a / 255.0, 0x46 / 255.0);
            cairo_fill(cr);
        }
        const double c = close.width / 2.0;
        const double arm = 4.5;
        cairo_set_line_width(cr, 1.5);
        const std::uint32_t cross = t.close_hot ? 0xf0f2f5 : 0x8b929f;
        cairo_set_source_rgb(cr, (cross >> 16) / 255.0, (cross >> 8 & 0xFF) / 255.0, (cross & 0xFF) / 255.0);
        cairo_move_to(cr, close.x + c - arm, close.y + c - arm);
        cairo_line_to(cr, close.x + c + arm, close.y + c + arm);
        cairo_move_to(cr, close.x + c + arm, close.y + c - arm);
        cairo_line_to(cr, close.x + c - arm, close.y + c + arm);
        cairo_stroke(cr);
        return TRUE;
    }

    bool in_close(double x, double y) {
        const GdkRectangle close = toast_close_box();
        return x >= close.x && x < close.x + close.width && y >= close.y && y < close.y + close.height;
    }

    // A frame of all the notifications: they come, go, and move to their places.
    gboolean step_toasts(gpointer) {
        const gint64 now = g_get_monotonic_time();
        const double dt = std::clamp(static_cast<double>(now - toast_last) / 1e6, 0.0, 0.1);
        toast_last = now;
        std::erase_if(gtk_toasts, [](const auto& t) {
            return t->life.gone();
        });
        // Too many: the oldest go.
        auto staying = std::ranges::count_if(gtk_toasts, [](const auto& t) {
            return t->life.going < 0;
        });
        for (auto& t : gtk_toasts) {
            if (staying <= static_cast<std::ptrdiff_t>(max_toasts)) {
                break;
            }
            if (t->life.going < 0) {
                t->life.close();
                --staying;
            }
        }
        if (gtk_toasts.empty()) {
            toast_source = 0;
            return G_SOURCE_REMOVE;
        }
        // The work area of the main screen of their display.
        GdkDisplay* display = gtk_widget_get_display(gtk_toasts.back()->window);
        GdkRectangle area {0, 0, 1280, 720};
        GdkMonitor* monitor = gdk_display_get_primary_monitor(display);
        if (!monitor) {
            monitor = gdk_display_get_monitor(display, 0);
        }
        if (monitor) {
            gdk_monitor_get_workarea(monitor, &area);
        }
        const auto tops = toast_places(gtk_toasts.size(), toast_height, toast_gap, toast_margin, area.y + area.height);
        for (std::size_t i = 0; i < gtk_toasts.size(); ++i) {
            GtkToast& t = *gtk_toasts[i];
            t.life.step(dt, tops[i]);
            const auto x = static_cast<gint>(std::lround(area.x + area.width - toast_margin - toast_width +
                                                         t.life.slide() * 48));
            const auto y = static_cast<gint>(std::lround(t.life.y));
            gtk_widget_set_opacity(t.window, t.life.opacity());
            if (x != t.x || y != t.y) {
                const bool first = t.x < 0;
                t.x = x;
                t.y = y;
                gtk_window_move(GTK_WINDOW(t.window), x, y);
                if (first) {
                    gtk_widget_show_all(t.window);
                }
            }
        }
        return G_SOURCE_CONTINUE;
    }

} // namespace

bool in_front(void* window) {
    return window && gtk_window_is_active(GTK_WINDOW(window));
}

void raise(void* window) {
    if (window) {
        gtk_window_deiconify(GTK_WINDOW(window));
        gtk_window_present(GTK_WINDOW(window));
    }
}

void toast(Toast toast) {
    GdkScreen* screen = toast_screen();
    if (!screen) {
        desktop_notification(toast);
        return;
    }
    auto t = std::make_unique<GtkToast>();
    t->toast = std::move(toast);
    // A popup: placed where it is told, above the others, and not a window of the taskbar (nor of Alt+Tab).
    GtkWidget* window = gtk_window_new(GTK_WINDOW_POPUP);
    t->window = window;
    gtk_window_set_screen(GTK_WINDOW(window), screen);
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    gtk_window_set_accept_focus(GTK_WINDOW(window), FALSE);
    if (GdkVisual* visual = gdk_screen_get_rgba_visual(screen); visual && gdk_screen_is_composited(screen)) {
        gtk_widget_set_visual(window, visual);
        t->round = true;
    }
    gtk_widget_set_app_paintable(window, TRUE);
    gtk_widget_set_size_request(window, static_cast<gint>(toast_width), static_cast<gint>(toast_height));
    gtk_widget_add_events(window, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                      GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    GtkToast* const raw = t.get();
    g_signal_connect(window, "draw", G_CALLBACK(draw_toast), raw);
    g_signal_connect(window, "enter-notify-event", G_CALLBACK(+[](GtkWidget*, GdkEventCrossing*, gpointer data) {
                         static_cast<GtkToast*>(data)->life.hovered = true;
                         return FALSE;
                     }),
                     raw);
    g_signal_connect(window, "leave-notify-event",
                     G_CALLBACK(+[](GtkWidget* widget, GdkEventCrossing* event, gpointer data) {
                         // Not when it goes onto a part of the window itself.
                         if (event->detail != GDK_NOTIFY_INFERIOR) {
                             auto* t = static_cast<GtkToast*>(data);
                             t->life.hovered = false;
                             if (t->close_hot) {
                                 t->close_hot = false;
                                 gtk_widget_queue_draw(widget);
                             }
                         }
                         return FALSE;
                     }),
                     raw);
    g_signal_connect(window, "motion-notify-event",
                     G_CALLBACK(+[](GtkWidget* widget, GdkEventMotion* event, gpointer data) {
                         auto* t = static_cast<GtkToast*>(data);
                         t->life.hovered = true;
                         if (const bool hot = in_close(event->x, event->y); hot != t->close_hot) {
                             t->close_hot = hot;
                             gtk_widget_queue_draw(widget);
                         }
                         return FALSE;
                     }),
                     raw);
    g_signal_connect(window, "button-release-event",
                     G_CALLBACK(+[](GtkWidget*, GdkEventButton* event, gpointer data) {
                         auto* t = static_cast<GtkToast*>(data);
                         if (event->button != 1 || t->life.going >= 0) {
                             return TRUE;
                         }
                         t->life.close();
                         // Not the ×: what it is about.
                         if (!in_close(event->x, event->y) && t->toast.clicked) {
                             // A copy: what it does may show another notification.
                             const auto clicked = t->toast.clicked;
                             clicked();
                         }
                         return TRUE;
                     }),
                     raw);
    gtk_window_resize(GTK_WINDOW(window), static_cast<gint>(toast_width), static_cast<gint>(toast_height));
    gtk_toasts.push_back(std::move(t));
    if (!toast_source) {
        toast_last = g_get_monotonic_time();
        toast_source = g_timeout_add(15, step_toasts, nullptr);
    }
}

#else
bool available() {
    return false;
}

void set_dispatcher(std::function<void(std::function<void()>)>) {
}

bool alert(std::string_view, std::chrono::milliseconds, std::stop_token) {
    return false;
}

Answer ask(std::string_view, std::string_view, std::string_view, std::chrono::milliseconds, std::stop_token) {
    return Answer::None;
}

void fly(const image::DecodedImage&, std::chrono::milliseconds, std::stop_token) {
}

void nudge(void*) {
}

bool in_front(void*) {
    return false;
}

void raise(void*) {
}

void toast(Toast) {
}

void flash(void*) {
}
#endif

} // namespace zchat::popup
