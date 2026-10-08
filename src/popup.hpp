#pragma once

#include "image.hpp"

#include <chrono>
#include <functional>
#include <stop_token>
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

} // namespace zchat::popup
