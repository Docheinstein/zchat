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
    Art = 'A',     // a picture drawn with characters: the text is its rows, separated by '\n'
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
// The size of an Art drawing. At most about 2 KB: bigger datagrams are less likely to make it through.
inline constexpr std::size_t max_art_cols = 64;
inline constexpr std::size_t max_art_rows = 32;

std::string encode(const Packet& packet);

// Parses a datagram; returns nullopt for anything that is not a well-formed zchat packet.
// Name and text are sanitized so they are safe to print.
std::optional<Packet> decode(std::string_view data);

} // namespace zchat
