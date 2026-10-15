// The hub on Windows: the window, OpenGL, the keyboard and mouse, the text, and the pipe to zchat. The whole hub is
// this one compilation unit: the game (hub.cpp) and the rest are included here, Handmade Hero style, so it builds in
// one go with nothing but the system's libraries.

#include <windows.h>

#include "hub.cpp"

#include <atomic>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

// --- OpenGL: a context of version 3.3 (core), with 4x multisampling, made the long way (WGL's ARB extensions need
// a context to be asked for, so a first one is made on a window of its own and thrown away).

#define WGL_DRAW_TO_WINDOW_ARB 0x2001
#define WGL_SUPPORT_OPENGL_ARB 0x2010
#define WGL_DOUBLE_BUFFER_ARB 0x2011
#define WGL_PIXEL_TYPE_ARB 0x2013
#define WGL_TYPE_RGBA_ARB 0x202B
#define WGL_COLOR_BITS_ARB 0x2014
#define WGL_DEPTH_BITS_ARB 0x2022
#define WGL_STENCIL_BITS_ARB 0x2023
#define WGL_SAMPLE_BUFFERS_ARB 0x2041
#define WGL_SAMPLES_ARB 0x2042
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

using ChoosePixelFormatARB = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using CreateContextAttribsARB = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using SwapIntervalEXT = BOOL(WINAPI*)(int);

static ChoosePixelFormatARB wglChoosePixelFormatARB_ = nullptr;
static CreateContextAttribsARB wglCreateContextAttribsARB_ = nullptr;
static SwapIntervalEXT wglSwapIntervalEXT_ = nullptr;

static void load_wgl_extensions() {
    WNDCLASSW cls {};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"zchat-hub-dummy";
    RegisterClassW(&cls);
    HWND window = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 1, 1, nullptr, nullptr, cls.hInstance, nullptr);
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd {};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC context = wglCreateContext(dc);
    wglMakeCurrent(dc, context);
    wglChoosePixelFormatARB_ = reinterpret_cast<ChoosePixelFormatARB>(wglGetProcAddress("wglChoosePixelFormatARB"));
    wglCreateContextAttribsARB_ =
        reinterpret_cast<CreateContextAttribsARB>(wglGetProcAddress("wglCreateContextAttribsARB"));
    wglSwapIntervalEXT_ = reinterpret_cast<SwapIntervalEXT>(wglGetProcAddress("wglSwapIntervalEXT"));
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
}

static bool make_context(HDC dc) {
    int format = 0;
    UINT count = 0;
    if (wglChoosePixelFormatARB_) {
        const int attributes[] = {WGL_DRAW_TO_WINDOW_ARB, 1, WGL_SUPPORT_OPENGL_ARB, 1, WGL_DOUBLE_BUFFER_ARB, 1,
                                  WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB, WGL_COLOR_BITS_ARB, 32, WGL_DEPTH_BITS_ARB, 24,
                                  WGL_STENCIL_BITS_ARB, 8, WGL_SAMPLE_BUFFERS_ARB, 1, WGL_SAMPLES_ARB, 4, 0};
        if (!wglChoosePixelFormatARB_(dc, attributes, nullptr, 1, &format, &count) || count == 0) {
            format = 0;
        }
    }
    PIXELFORMATDESCRIPTOR pfd {};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    if (format == 0) {
        // Without multisampling, then.
        format = ChoosePixelFormat(dc, &pfd);
    }
    DescribePixelFormat(dc, format, sizeof pfd, &pfd);
    if (!SetPixelFormat(dc, format, &pfd) || !wglCreateContextAttribsARB_) {
        return false;
    }
    const int attributes[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                              WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    HGLRC context = wglCreateContextAttribsARB_(dc, nullptr, attributes);
    if (!context || !wglMakeCurrent(dc, context)) {
        return false;
    }
    bool ok = true;
#define HUB_GL_LOAD(ret, name, params)                                                                                 \
    name = reinterpret_cast<ret(APIENTRY*) params>(wglGetProcAddress(#name));                                          \
    ok = ok && name != nullptr;
    HUB_GL_FUNCTIONS(HUB_GL_LOAD)
#undef HUB_GL_LOAD
    if (wglSwapIntervalEXT_) {
        wglSwapIntervalEXT_(1);
    }
    return ok;
}

// --- Text, drawn by GDI in a bitmap of its own (white, its coverage in alpha).

static std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool platform_text(const std::string& utf8, int pixel_height, bool bold, TextBitmap& out) {
    const std::wstring text = wide(utf8);
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-pixel_height, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    const HGDIOBJ old_font = SelectObject(dc, font);
    SIZE size {};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    const int w = std::max<int>(1, size.cx + 2), h = std::max<int>(1, size.cy);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }
    const HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, 1, 0, text.c_str(), static_cast<int>(text.size()));
    GdiFlush();
    out.width = w;
    out.height = h;
    out.rgba.assign(static_cast<std::size_t>(w) * h * 4, 255);
    const auto* px = static_cast<const std::uint8_t*>(bits);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        // Grey on black: any channel is the coverage.
        out.rgba[i * 4 + 3] = std::max(px[i * 4], std::max(px[i * 4 + 1], px[i * 4 + 2]));
    }
    SelectObject(dc, old_bitmap);
    SelectObject(dc, old_font);
    DeleteObject(bitmap);
    DeleteObject(font);
    DeleteDC(dc);
    return true;
}

// --- The pipe to zchat: its lines are read on a thread of their own, and wait for the game in a queue.

static HANDLE pipe_in = INVALID_HANDLE_VALUE;
static HANDLE pipe_out = INVALID_HANDLE_VALUE;
static bool connected = false;
static std::mutex lines_mutex;
static std::deque<std::string> lines;
static std::atomic<bool> running {true};

static void read_pipe() {
    std::string pending;
    char buf[4096];
    for (DWORD n = 0; ReadFile(pipe_in, buf, sizeof buf, &n, nullptr) && n > 0;) {
        pending.append(buf, n);
        for (std::size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            std::scoped_lock lock(lines_mutex);
            lines.push_back(std::move(line));
        }
    }
    // zchat is gone: so are we.
    std::scoped_lock lock(lines_mutex);
    lines.push_back("quit");
}

void platform_send(const std::string& line) {
    if (!connected) {
        return;
    }
    const std::string data = line + "\n";
    DWORD written = 0;
    WriteFile(pipe_out, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
}

bool platform_receive(std::string& line) {
    std::scoped_lock lock(lines_mutex);
    if (lines.empty()) {
        return false;
    }
    line = std::move(lines.front());
    lines.pop_front();
    return true;
}

bool platform_connected() {
    return connected;
}

void platform_quit() {
    running = false;
}

// --- The window and its input.

static Input input;
static bool resized = true;

static int key_of(WPARAM vk) {
    switch (vk) {
    case 'W': return key_w;
    case 'A': return key_a;
    case 'S': return key_s;
    case 'D': return key_d;
    case 'Q': return key_q;
    case 'E': return key_e;
    case VK_SHIFT: return key_shift;
    case VK_SPACE: return key_space;
    case VK_RETURN: return key_enter;
    case VK_ESCAPE: return key_escape;
    case VK_BACK: return key_backspace;
    case VK_UP: return key_up;
    case VK_DOWN: return key_down;
    case VK_LEFT: return key_left;
    case VK_RIGHT: return key_right;
    default: return -1;
    }
}

static void set_button(Button& b, bool down) {
    if (b.down != down) {
        if (down) {
            b.pressed = true;
        } else {
            b.released = true;
        }
    }
    b.down = down;
}

static wchar_t high_surrogate = 0;

static void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

static LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CLOSE:
        running = false;
        return 0;
    case WM_SIZE:
        input.width = std::max(1, static_cast<int>(LOWORD(lparam)));
        input.height = std::max(1, static_cast<int>(HIWORD(lparam)));
        resized = true;
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        const int key = key_of(wparam);
        if (key >= 0) {
            set_button(input.keys[key], msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        }
        if (msg == WM_SYSKEYDOWN && wparam == VK_F4) {
            running = false;
        }
        return 0;
    }
    case WM_CHAR: {
        const auto unit = static_cast<wchar_t>(wparam);
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            high_surrogate = unit;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (high_surrogate) {
                append_utf8(input.typed, 0x10000 + ((static_cast<char32_t>(high_surrogate) - 0xD800) << 10) +
                                             (static_cast<char32_t>(unit) - 0xDC00));
            }
            high_surrogate = 0;
        } else if (unit >= 0x20 && unit != 0x7F) {
            append_utf8(input.typed, unit);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        SetCapture(window);
        set_button(msg == WM_LBUTTONDOWN ? input.mouse_left : input.mouse_right, true);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        set_button(msg == WM_LBUTTONUP ? input.mouse_left : input.mouse_right, false);
        if (!input.mouse_left.down && !input.mouse_right.down) {
            ReleaseCapture();
        }
        return 0;
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
        const float y = static_cast<float>(static_cast<short>(HIWORD(lparam)));
        input.mouse_dx += x - input.mouse_x;
        input.mouse_dy += y - input.mouse_y;
        input.mouse_x = x;
        input.mouse_y = y;
        return 0;
    }
    case WM_MOUSEWHEEL:
        input.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        return 0;
    case WM_KILLFOCUS:
        // Keys let go elsewhere would stay down.
        for (Button& b : input.keys) {
            set_button(b, false);
        }
        set_button(input.mouse_left, false);
        set_button(input.mouse_right, false);
        input.focused = false;
        return 0;
    case WM_SETFOCUS:
        input.focused = true;
        return 0;
    default:
        return DefWindowProcW(window, msg, wparam, lparam);
    }
}

static Hub hub;

// --- For working on the hub: a picture of a frame, in a .bmp, with the player and the camera put somewhere.
//   zchat_hub --screenshot FILE.bmp [--at X Z] [--look YAW PITCH DISTANCE] [--bowl LANE POWER] [--frames N]
// draws N frames (60 by default; a throw goes on for as many, or the ball waits, with a POWER below 0), saves the
// last and quits.

struct Shot {
    std::wstring path;
    int frames = 60;
    bool at = false, look = false;
    float x = 0, z = 0, yaw = 0, pitch = 0, distance = 0;
    int lane = -1;
    float power = 0.7f;
};

static void save_bmp(const std::wstring& path, int w, int h) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    const int row = (w * 3 + 3) & ~3;
    std::vector<std::uint8_t> file(54 + static_cast<std::size_t>(row) * h, 0);
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            file[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, static_cast<std::uint32_t>(file.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<std::uint32_t>(w));
    put32(22, static_cast<std::uint32_t>(h));
    file[26] = 1;
    file[28] = 24;
    // OpenGL's rows are bottom up, as a .bmp's are.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * w + x) * 4];
            std::uint8_t* q = &file[54 + static_cast<std::size_t>(y) * row + x * 3];
            q[0] = p[2];
            q[1] = p[1];
            q[2] = p[0];
        }
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    WriteFile(f, file.data(), static_cast<DWORD>(file.size()), &written, nullptr);
    CloseHandle(f);
}

static Shot parse_shot() {
    Shot shot;
    const auto arg = [](int i) {
        return i < __argc ? std::wstring(__wargv[i]) : std::wstring();
    };
    const auto num = [&](int i) {
        return static_cast<float>(_wtof(arg(i).c_str()));
    };
    for (int i = 1; i < __argc; ++i) {
        const std::wstring a = arg(i);
        if (a == L"--screenshot") {
            shot.path = arg(++i);
        } else if (a == L"--frames") {
            shot.frames = static_cast<int>(num(++i));
        } else if (a == L"--at") {
            shot.at = true;
            shot.x = num(i + 1);
            shot.z = num(i + 2);
            i += 2;
        } else if (a == L"--look") {
            shot.look = true;
            shot.yaw = num(i + 1);
            shot.pitch = num(i + 2);
            shot.distance = num(i + 3);
            i += 3;
        } else if (a == L"--bowl") {
            shot.lane = static_cast<int>(num(i + 1));
            shot.power = num(i + 2);
            i += 2;
        }
    }
    return shot;
}

// Puts the player and the camera as the shot says, before a frame.
static void stage_shot(const Shot& shot, int frame) {
    if (frame == 1) {
        if (shot.at) {
            hub.me.x = shot.x;
            hub.me.z = shot.z;
        }
        if (shot.look) {
            hub.cam_yaw = shot.yaw;
            hub.cam_pitch = shot.pitch;
            hub.cam_distance = shot.distance;
        }
        if (shot.lane == 0 || shot.lane == 1) {
            hub.my_lane = shot.lane;
            hub.lanes[shot.lane] = {};
            hub.lanes[shot.lane].mine = true;
            hub.lanes[shot.lane].owner_name = "You";
            start_game(hub.lanes[shot.lane]);
            // (A power below 0: aiming, not thrown.)
            if (shot.power >= 0) {
                hub.lanes[shot.lane].power = shot.power;
                throw_ball(hub.lanes[shot.lane]);
            }
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // zchat gives us pipes: our standard input and output. Alone (started by hand), there are none.
    pipe_in = GetStdHandle(STD_INPUT_HANDLE);
    pipe_out = GetStdHandle(STD_OUTPUT_HANDLE);
    connected = pipe_in && pipe_in != INVALID_HANDLE_VALUE && GetFileType(pipe_in) == FILE_TYPE_PIPE;

    load_wgl_extensions();
    WNDCLASSEXW cls {};
    cls.cbSize = sizeof cls;
    cls.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(32512));
    cls.lpszClassName = L"zchat-hub";
    RegisterClassExW(&cls);
    const UINT dpi = GetDpiForSystem();
    RECT rect {0, 0, MulDiv(1280, static_cast<int>(dpi), 96), MulDiv(760, static_cast<int>(dpi), 96)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"zchat hub", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                  instance, nullptr);
    HDC dc = GetDC(window);
    if (!window || !make_context(dc)) {
        MessageBoxW(nullptr, L"The hub needs OpenGL 3.3: update the graphics driver.", L"zchat hub", MB_ICONERROR);
        return 1;
    }
    ShowWindow(window, show);
    SetForegroundWindow(window);

    std::thread reader;
    if (connected) {
        reader = std::thread(read_pipe);
        reader.detach();
    }

    const Shot shot = parse_shot();
    int frame = 0;

    LARGE_INTEGER frequency, last, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&last);
    const LARGE_INTEGER start = last;
    while (running) {
        for (Button& b : input.keys) {
            b.pressed = b.released = false;
        }
        input.mouse_left.pressed = input.mouse_left.released = false;
        input.mouse_right.pressed = input.mouse_right.released = false;
        input.mouse_dx = input.mouse_dy = input.wheel = 0;
        input.typed.clear();
        for (MSG msg; PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        QueryPerformanceCounter(&now);
        input.dt = std::min(0.1f, static_cast<float>(now.QuadPart - last.QuadPart) /
                                      static_cast<float>(frequency.QuadPart));
        input.time = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
        last = now;
        if (!shot.path.empty()) {
            // A steady 60 frames a second, whatever the machine.
            input.dt = 1.0f / 60;
            input.time = frame / 60.0;
            stage_shot(shot, frame);
        }
        hub_frame(hub, input);
        if (!shot.path.empty() && ++frame >= shot.frames) {
            save_bmp(shot.path, input.width, input.height);
            running = false;
        }
        SwapBuffers(dc);
    }
    // The pipe's thread may be waiting on zchat: the process ends anyway.
    ExitProcess(0);
}
