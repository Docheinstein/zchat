#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace zchat::channel {

// The channel everybody is always in: it is not a Channel, cannot be created, deleted or left, and its messages are
// the plain Message packets older versions know.
inline constexpr std::string_view general = "general";

// Where a window shows what the games say, so the chat stays a chat (see Screen::games_apart()). Not a Channel
// either: each zchat has its own, as the games' lines are its own, and nobody can create one called so.
inline constexpr std::string_view games_log = "games-log";

inline constexpr std::size_t max_name = 32;
inline constexpr std::size_t max_members = 48;

// A channel besides general. There is no server: every zchat keeps what it knows of each channel, and the members
// tell the others (see PacketType::ChannelState). A change makes a new version, and the newest one wins; see
// accepts() for who may make one.
struct Channel {
    std::string name;
    std::uint64_t version = 1;
    // Public ones anybody can join; private ones only whoever a member adds.
    bool is_public = true;
    // Deleted ones are kept, so an old version heard later does not bring them back.
    bool deleted = false;
    // Who created it: they alone can delete it. Users, not senders: see Chat::set_user().
    std::uint64_t owner = 0;
    // Who made this version.
    std::uint64_t author = 0;
    std::set<std::uint64_t> members;

    bool has(std::uint64_t user) const {
        return members.contains(user);
    }
};

// The name of a channel, lowercase: letters, digits, '-' and '_', up to max_name, as typed with or without a '#' in
// front. Empty when it is not one.
std::string clean_name(std::string_view name);

// "NAME VERSION public|private live|deleted OWNER AUTHOR MEMBER...", the ids in hex: as sent, and as saved.
std::string encode(const Channel& channel);
std::optional<Channel> decode(std::string_view text);

// Whether a new version of a channel is taken over the one known (if any): it must be newer (or as new, from a
// higher author, so everybody picks the same), and its author must be allowed to make it. Anybody in a channel can
// add or remove people, anybody can join a public one (or leave any), and only the owner can delete it or change
// whether it is public.
bool accepts(const std::optional<Channel>& known, const Channel& incoming);

} // namespace zchat::channel
