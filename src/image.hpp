#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
// behind the blocks ('|' for none), and '"' makes the symbol after it texture: shown as itself, not as a block.
// Shade blocks (░▒▓) are texture too. The first row is the palette of the picture, '$' then 3 characters per color,
// which the color codes refer to. Every row starts without a color, at full brightness and without a background,
// and a row that would be too long for a packet is drawn more simply. texture, from 0 to 100, is how much of the
// picture is drawn with symbols instead of blocks: 0 none, about 15 the dark and mostly black parts, 100 all of it
// (no blocks at all). On failure, returns why.
std::expected<std::string, std::string> to_ascii(const std::filesystem::path& path, std::size_t max_cols,
                                                 std::size_t max_rows, int texture = 0);

// The same, from the bytes of an image file (e.g. the JPEG of a received picture).
std::expected<std::string, std::string> to_ascii_data(std::string_view encoded, std::size_t max_cols,
                                                      std::size_t max_rows, int texture = 0);

// The bytes of an image file, or why they cannot be read.
std::expected<std::string, std::string> read_image_file(const std::filesystem::path& path);

// An image file decoded: its pixels, RGBA, row by row.
struct DecodedImage {
    std::unique_ptr<unsigned char, void (*)(void*)> pixels;
    int width = 0;
    int height = 0;
};
std::expected<DecodedImage, std::string> decode_image(std::string_view encoded);

// A real picture to send (see PacketType::Image), made to fit in a packet, and at most max_size pixels wide and tall:
// "jpeg W H", '\n' and a JPEG in base64 (on black where it is transparent); for an animated GIF, "gif W H" and the
// GIF itself when it is small enough, or else "anim W H" and a line per frame, "DELAY BASE64" (a JPEG, and how long
// it is shown in milliseconds). On failure, returns why.
// still_of_animation (if given) tells whether it is the first frame of an animation too long to send whole.
std::expected<std::string, std::string> encode_picture(const std::filesystem::path& path, int max_size,
                                                       bool* still_of_animation = nullptr);

// An avatar, as a picture like encode_picture()'s, square: a still image cropped to its middle, 256 pixels; an
// animated GIF as it is when it is small enough (shown cropped), or else as square JPEG frames. At most
// max_avatar_bytes. On failure, returns why.
std::expected<std::string, std::string> encode_avatar(const std::filesystem::path& path);

// A received picture: its size, and its frames (one, or more for an animation), each an image file in base64 (as
// data: URLs want it) shown for its delay in milliseconds; and the bytes of the first, to draw it with characters.
struct Picture {
    struct Frame {
        std::string mime;
        std::string base64;
        int delay_ms = 0;
    };
    int width = 0;
    int height = 0;
    std::vector<Frame> frames;
    std::string first;
};
std::optional<Picture> parse_picture(std::string_view text);

// Turns ASCII art from to_ascii() into what to print. With colors, each colored symbol becomes a solid block of its
// color, darker for the symbols with less ink and dimmed by the brightness codes, and blocks are shown in their color
// on their background; without, each character becomes the ASCII symbol as bright as it looks. Art without color
// codes is shown as it is.
std::string render(std::string_view art, bool colors);

} // namespace zchat::image
