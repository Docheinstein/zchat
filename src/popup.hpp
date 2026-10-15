#pragma once

#include "image.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace zchat::popup {

// Windows of their own for trills (see /trill) and Pokémon challenges, above all the others on the screen. On Windows they are made on the
// thread that asks, which waits for them; on Linux with GTK 3, on the thread of the zchat window (see
// set_dispatcher()), so only while it is open. Elsewhere there are none (see available()), and zchat does without.

// Whether popups can be shown now.
bool available();

// On Linux, what runs work on the thread of the window's GTK loop: set by the window, and emptied when it closes.
void set_dispatcher(std::function<void(std::function<void()>)> dispatch);

// A little window saying text, with a STOP button, running around the screen for wait, or until stop is requested.
// Returns whether STOP was pressed.
bool alert(std::string_view text, std::chrono::milliseconds wait, std::stop_token stop);

// What ask() was answered.
enum class Answer { None, Yes, No };

// A question that cannot be missed, like a challenge: a big window in the middle of the screen, above every window,
// flashing, with the buttons yes and no, until one is pressed, wait is over (the seconds left are shown), or stop is
// requested. Clicked without taking the focus from what the user types in. Returns the button pressed, or None.
Answer ask(std::string_view text, std::string_view yes, std::string_view no, std::chrono::milliseconds wait,
           std::stop_token stop);

// A picture flying around the screen, big and shaking, above every window, for duration, or until stop is requested.
// Clicks go through it.
void fly(const image::DecodedImage& picture, std::chrono::milliseconds duration, std::stop_token stop);

// Brings a window to the front (shown again if it is minimized) and shakes it, like MSN's nudge: an HWND on Windows,
// a GtkWindow* on Linux. Takes about half a second.
void nudge(void* window);

// Whether a window (as for nudge()) is the one in front, that the user types in. On its thread.
bool in_front(void* window);

// Brings a window (as for nudge()) to the front, shown again if it is minimized, as after a click on a notification
// about it. On its thread.
void raise(void* window);

// A desktop notification, as chat programs show them (see toast()).
struct Toast {
    // Small, above the rest: where it comes from (e.g. "#general", a game).
    std::string where;
    // Who or what, in bold, and what they say. Plain text.
    std::string title;
    std::string text;
    // Its color, 0xRRGGBB: the stripe on its left, and the circle where there is no picture.
    std::uint32_t accent = 0x6ea8ff;
    // The picture of whom it is from (their avatar), shown in a circle; null for none (then the title's initial).
    std::shared_ptr<const image::DecodedImage> picture;
    // What clicking it does (besides closing it), on the thread it was shown on.
    std::function<void()> clicked;
};

// Shows a notification: a little window in the bottom right corner of the screen, above the others, without taking
// the focus, that goes away by itself after a few seconds (not while the mouse is on it), or when clicked. Several
// stack up, the newest at the bottom.
// On the thread of the zchat window (on Linux, that of GTK's loop; see set_dispatcher()), whose loop runs it. On
// Linux under Wayland, where a window cannot choose where it goes, it is drawn through Xwayland; without it, the
// notification is the desktop's own (without clicked).
void toast(Toast toast);

// Flashes a window's button in the taskbar until it is looked at again, if it is not in front: an HWND on Windows, a
// GtkWindow* on Linux. Thread-safe.
void flash(void* window);

} // namespace zchat::popup
