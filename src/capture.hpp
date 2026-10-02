#pragma once

#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

// Screen capture for /spy: takes a picture of each of this machine's monitors, by running the system's own
// screenshot tool (see capture.cpp). It is used only after the person at this machine has agreed to be spied on
// (see spy.cpp): zchat never captures a screen on its own.
namespace zchat::capture {

// The pictures of all the monitors, as temporary image files (one per monitor where the tool can tell them apart,
// or one of the whole desktop otherwise), newest capture in its own folder; or why none could be taken (no tool,
// no display, it was killed...). The caller sends them and may delete them: they live in a temp folder of their own.
std::expected<std::vector<std::filesystem::path>, std::string> capture_screens(std::stop_token stop);

} // namespace zchat::capture
