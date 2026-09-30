#include "protocol.hpp"

#include "text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>

namespace zchat {

namespace {

    constexpr std::string_view magic = "ZCHAT1";

    template <typename T>
    bool parse_number(std::string_view s, T& value, int base) {
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
        return ec == std::errc {} && ptr == s.data() + s.size() && !s.empty();
    }

    // Like text::sanitize(), but for the text of a packet: the rows of an Art drawing keep their line breaks.
    std::string sanitize_text(PacketType type, std::string_view text) {
        if (type == PacketType::Image || type == PacketType::Chunk) {
            // Its first line, then nothing but base64 (and, for an animation, the delay and line of each frame; for
            // a chunk, what its piece has of those).
            const std::size_t max = type == PacketType::Image ? max_image_bytes : max_chunk_bytes;
            const auto nl = text.find('\n');
            std::string out = text::sanitize(text.substr(0, nl), max_image_head_bytes);
            if (nl == std::string_view::npos) {
                return out;
            }
            out += '\n';
            const std::size_t head = out.size();
            out.reserve(head + std::min(max, text.size() - nl - 1));
            for (const char c : text.substr(nl + 1)) {
                if (out.size() - head >= max) {
                    break;
                }
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' || c == '=' || c == ' ' ||
                    c == '\n') {
                    out += c;
                }
            }
            return out;
        }
        if (type == PacketType::Resend) {
            return text::sanitize(text, max_resend_bytes);
        }
        if (type != PacketType::Art) {
            return text::sanitize(text, max_text_bytes);
        }
        std::string out;
        // The rows of the drawing, and the palette row before them.
        for (std::size_t row = 0; row < max_art_rows + 1 && !text.empty(); ++row) {
            const auto nl = text.find('\n');
            out += row == 0 ? "" : "\n";
            out += text::sanitize(text.substr(0, nl), max_art_row_bytes);
            text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        }
        return out;
    }

} // namespace

std::string encode(const Packet& packet) {
    return std::format("{}\n{}\n{:x}\n{}\n{}\n{}", magic, static_cast<char>(packet.type), packet.sender, packet.seq,
                       text::sanitize(packet.name, max_name_bytes), sanitize_text(packet.type, packet.text));
}

std::optional<Packet> decode(std::string_view data) {
    // Split into the first five fields; the text is everything after them.
    std::array<std::string_view, 6> fields;
    for (std::size_t i = 0; i < 5; ++i) {
        const auto nl = data.find('\n');
        if (nl == std::string_view::npos) {
            return std::nullopt;
        }
        fields[i] = data.substr(0, nl);
        data.remove_prefix(nl + 1);
    }
    fields[5] = data;

    if (fields[0] != magic || fields[1].size() != 1) {
        return std::nullopt;
    }
    Packet packet;
    switch (fields[1][0]) {
    case 'J':
        packet.type = PacketType::Join;
        break;
    case 'H':
        packet.type = PacketType::Here;
        break;
    case 'M':
        packet.type = PacketType::Message;
        break;
    case 'L':
        packet.type = PacketType::Leave;
        break;
    case 'P':
    case 'A': // black and white, from older versions
        packet.type = PacketType::Art;
        break;
    case 'G':
        packet.type = PacketType::Game;
        break;
    case 'I':
        packet.type = PacketType::Image;
        break;
    case 'K':
        packet.type = PacketType::Chunk;
        break;
    case 'R':
        packet.type = PacketType::Resend;
        break;
    case 'O':
        packet.type = PacketType::Offer;
        break;
    default:
        return std::nullopt;
    }
    if (!parse_number(fields[2], packet.sender, 16) || !parse_number(fields[3], packet.seq, 10)) {
        return std::nullopt;
    }
    packet.name = text::sanitize(fields[4], max_name_bytes);
    packet.text = sanitize_text(packet.type, fields[5]);
    if (packet.name.empty()) {
        return std::nullopt;
    }
    return packet;
}

} // namespace zchat
