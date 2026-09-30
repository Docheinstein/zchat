#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace zchat::image {

// The file named by a line of input, the way terminals type it when a file is dragged and dropped on them:
// a plain path, a quoted one ("C:\My Pictures\cat.png" or '/home/me/my cat.png'), or a file:// URL. Outside
// Windows it is unquoted the way a shell does, so /home/me/my\ cat.png and '/home/me/it'\''s.png' work too.
// Empty when the line is empty.
std::filesystem::path parse_path(std::string_view line);

// Whether a line of input is nothing but the path of an existing image file, as when one is dropped on the
// terminal. Only the file name is looked at, not its contents.
bool is_dropped_image(std::string_view line);

// Turns an image file into colored ASCII art that fits in max_cols x max_rows characters, keeping its
// proportions. The rows are separated by '\n': the characters of the drawing are the ASCII symbols " .:-=+*#%@",
// from dark to bright, and blocks (▀▄▌▐, eighths like ▁▂▃ and ▏▎▍, quadrants like ▘▚▙) where a character has parts
// of two colors; codes before them set their look: a letter or digit their color, one of ()[]{}<>^~;? how bright
// they are, from the dimmest to full ('`' for black), and '&' followed by a color and a brightness code the color
// behind the blocks ('|' for none). The first row is the palette of the picture, '$' then 3 characters per color,
// which the color codes refer to. Every row starts without a color, at full brightness and without a background,
// and a row that would be too long for a packet is drawn more simply. On failure, returns why.
std::expected<std::string, std::string> to_ascii(const std::filesystem::path& path, std::size_t max_cols,
                                                 std::size_t max_rows);

// Turns ASCII art from to_ascii() into what to print. With colors, each colored symbol becomes a solid block of its
// color, darker for the symbols with less ink and dimmed by the brightness codes, and blocks are shown in their color
// on their background; without, each character becomes the ASCII symbol as bright as it looks. Art without color
// codes is shown as it is.
std::string render(std::string_view art, bool colors);

} // namespace zchat::image
