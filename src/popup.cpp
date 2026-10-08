#include "popup.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
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
#endif

} // namespace zchat::popup
