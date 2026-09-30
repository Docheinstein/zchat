#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace zchat {

// Wire format (one UDP datagram, fields separated by '\n'):
//   ZCHAT1 \n <type> \n <sender id, hex> \n <sequence number> \n <name> \n <text>
enum class PacketType : char {
    Join = 'J',    // a peer just started; everyone answers with Here
    Here = 'H',    // periodic heartbeat, also the answer to Join
    Message = 'M', // a chat line
    Leave = 'L',   // a peer is quitting, or, when the text is a new sender id, changing color
    // A colored picture drawn with characters: the text is its rows, separated by '\n', see image::to_ascii().
    // Versions before colors sent black and white ones as 'A', which are still shown; they do not know 'P', so
    // they show nothing rather than the color codes.
    Art = 'P',
    // Something happening in a game played in the chat: the text starts with the game's name, see game::Games.
    // Older versions do not know 'G' and ignore it.
    Game = 'G',
    // A real picture: "jpeg WIDTH HEIGHT", then '\n' and the JPEG file in base64; or an animated GIF, as "gif W H"
    // and the GIF, or "anim W H" and a line per frame, see image::encode_picture().
    // Windows show it as it is; terminals draw it with characters. Older versions do not know 'I' and ignore it.
    // Also "png W H" and "bmp W H", with the file: pictures are sent as they are whenever they can be.
    Image = 'I',
    // A piece of an Image too big for one datagram: "ID INDEX COUNT", then '\n' and the piece. The pieces, in order,
    // are the Image's text; ID tells the pictures of a sender apart.
    Chunk = 'K',
    // Asks for pieces that did not arrive: "SENDER ID INDEX INDEX ...", SENDER in hex, as in packets. The sender of
    // the picture sends them again.
    Resend = 'R',
};

struct Packet {
    PacketType type = PacketType::Here;
    std::uint64_t sender = 0;
    std::uint64_t seq = 0;
    std::string name;
    std::string text;
};

inline constexpr std::size_t max_name_bytes = 48;
inline constexpr std::size_t max_text_bytes = 1000;
// The size of an Art drawing, in characters. With the color and brightness codes and Unicode blocks, a row takes up
// to five times as many bytes, so a drawing is at most about 10 KB (usually much less): bigger datagrams are less
// likely to make it through.
inline constexpr std::size_t max_art_cols = 64;
inline constexpr std::size_t max_art_rows = 32;
// A character can take a color code, a brightness code and a 3-byte UTF-8 character (a block): rows that would be
// longer are drawn with fewer of them, see image::to_ascii().
inline constexpr std::size_t max_art_row_bytes = 5 * max_art_cols;
// The size of an Image's text: files up to about 36 MB. Bigger than max_chunk_bytes, it is sent in Chunks.
inline constexpr std::size_t max_image_bytes = 48'000'000;
// The size of a Chunk's piece: with the rest of the packet it fits in one datagram (at most 64 KB).
inline constexpr std::size_t max_chunk_bytes = 48'000;
inline constexpr std::size_t max_chunks = (max_image_bytes + max_chunk_bytes - 1) / max_chunk_bytes;
inline constexpr std::size_t max_resend_bytes = 4000;

std::string encode(const Packet& packet);

// Parses a datagram; returns nullopt for anything that is not a well-formed zchat packet.
// Name and text are sanitized so they are safe to print.
std::optional<Packet> decode(std::string_view data);

} // namespace zchat
