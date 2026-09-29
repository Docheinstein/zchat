#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace zchat::image {

// The file named by a line of input, the way terminals type it when a file is dragged and dropped on them:
// a plain path, a quoted one ("C:\My Pictures\cat.png" or '/home/me/my cat.png'), one with backslash-escaped
// spaces (/home/me/my\ cat.png, not on Windows), or a file:// URL. Empty when the line is empty.
std::filesystem::path parse_path(std::string_view line);

// Whether a line of input is nothing but the path of an existing image file, as when one is dropped on the
// terminal. Only the file name is looked at, not its contents.
bool is_dropped_image(std::string_view line);

// Turns an image file into ASCII art that fits in max_cols x max_rows characters, keeping its proportions.
// The rows are separated by '\n', and use only printable ASCII. On failure, returns why.
std::expected<std::string, std::string> to_ascii(const std::filesystem::path& path, std::size_t max_cols,
                                                 std::size_t max_rows);

} // namespace zchat::image
