// What the platform layer (win32_hub.cpp) and the game (hub.cpp) say to each other, Handmade Hero style: the
// platform owns the window, OpenGL, the input and zchat's pipe, and calls hub_frame() once a frame with what
// happened; the game calls the few services below.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum Key {
    key_w,
    key_a,
    key_s,
    key_d,
    key_q,
    key_e,
    key_shift,
    key_space,
    key_enter,
    key_escape,
    key_backspace,
    key_up,
    key_down,
    key_left,
    key_right,
    key_count,
};

// A key or a mouse button: whether it is down, and whether it went down or up during this frame.
struct Button {
    bool down = false;
    bool pressed = false;
    bool released = false;
};

struct Input {
    // Seconds since the last frame (at most a tenth), and since the start.
    float dt = 0;
    double time = 0;
    // The size of the window's inside, in pixels.
    int width = 1;
    int height = 1;
    Button keys[key_count];
    Button mouse_left;
    Button mouse_right;
    // The pointer, in pixels from the top left; how it moved this frame; the wheel's turns this frame.
    float mouse_x = 0;
    float mouse_y = 0;
    float mouse_dx = 0;
    float mouse_dy = 0;
    float wheel = 0;
    // The text typed this frame (UTF-8, no control characters).
    std::string typed;
    bool focused = true;
};

// Some text, drawn white with its coverage in alpha, as RGBA bytes: for names, bubbles and the HUD.
struct TextBitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

// The services of the platform.
bool platform_text(const std::string& utf8, int pixel_height, bool bold, TextBitmap& out);
// A line to zchat (see hub.cpp for what they say), and the next one from it; false when there is none.
void platform_send(const std::string& line);
bool platform_receive(std::string& line);
// Whether zchat started us (with a pipe); alone, the hub is offline, for trying it out.
bool platform_connected();
void platform_quit();

// The game, once a frame: it updates and draws (the platform swaps the buffers after).
struct Hub;
void hub_frame(Hub& hub, const Input& input);
