#include "channel.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <vector>

namespace zchat::channel {

namespace {

    std::vector<std::string_view> words(std::string_view s) {
        std::vector<std::string_view> out;
        while (!s.empty()) {
            const auto space = s.find(' ');
            if (space != 0) {
                out.push_back(s.substr(0, space));
            }
            if (space == std::string_view::npos) {
                break;
            }
            s.remove_prefix(space + 1);
        }
        return out;
    }

    template <typename T>
    bool number(std::string_view s, T& value, int base) {
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
        return !s.empty() && ec == std::errc {} && ptr == s.data() + s.size();
    }

} // namespace

std::string clean_name(std::string_view name) {
    if (name.starts_with('#')) {
        name.remove_prefix(1);
    }
    if (name.empty() || name.size() > max_name) {
        return {};
    }
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80 || (!std::isalnum(u) && c != '-' && c != '_')) {
            return {};
        }
        out += static_cast<char>(std::tolower(u));
    }
    return out;
}

std::string encode(const Channel& channel) {
    std::string out = std::format("{} {} {} {} {:x} {:x}", channel.name, channel.version,
                                  channel.is_public ? "public" : "private", channel.deleted ? "deleted" : "live",
                                  channel.owner, channel.author);
    for (const auto member : channel.members) {
        out += std::format(" {:x}", member);
    }
    return out;
}

std::optional<Channel> decode(std::string_view text) {
    const auto w = words(text);
    if (w.size() < 6 || w.size() > 6 + max_members) {
        return std::nullopt;
    }
    Channel c;
    c.name = clean_name(w[0]);
    if (c.name.empty() || c.name == general || !number(w[1], c.version, 10) || c.version == 0 ||
        (w[2] != "public" && w[2] != "private") || (w[3] != "live" && w[3] != "deleted") ||
        !number(w[4], c.owner, 16) || !number(w[5], c.author, 16)) {
        return std::nullopt;
    }
    c.is_public = w[2] == "public";
    c.deleted = w[3] == "deleted";
    for (std::size_t i = 6; i < w.size(); ++i) {
        std::uint64_t member = 0;
        if (!number(w[i], member, 16) || member == 0) {
            return std::nullopt;
        }
        c.members.insert(member);
    }
    return c;
}

bool accepts(const std::optional<Channel>& known, const Channel& incoming) {
    if (!known) {
        // Nothing to go by: the first one heard.
        return true;
    }
    if (incoming.version < known->version ||
        (incoming.version == known->version && incoming.author <= known->author)) {
        return false;
    }
    const std::uint64_t author = incoming.author;
    // Recreating a deleted one makes a new channel: its creator is its owner.
    if (known->deleted) {
        return !incoming.deleted && incoming.owner == author;
    }
    if (incoming.deleted || incoming.is_public != known->is_public || incoming.owner != known->owner) {
        return author == known->owner;
    }
    if (known->has(author) || author == known->owner) {
        return true;
    }
    // Not in it: joining a public one, and nothing else.
    std::set<std::uint64_t> joined = known->members;
    joined.insert(author);
    return known->is_public && incoming.members == joined;
}

} // namespace zchat::channel
