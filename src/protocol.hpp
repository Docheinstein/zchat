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

std::string encode(const Packet& packet);

// Parses a datagram; returns nullopt for anything that is not a well-formed zchat packet.
// Name and text are sanitized so they are safe to print.
std::optional<Packet> decode(std::string_view data);

} // namespace zchat
