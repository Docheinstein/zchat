#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace zchat::world {

// The rooms of the world (see world.cpp): only the lobby for now. What is in them, and their size, is the window's
// own (see ui/index.html); zchat only tells who is where.
inline constexpr std::string_view lobby = "lobby";

// A room is at most this many tiles wide and tall: a place beyond is not believed.
inline constexpr int max_room_tiles = 64;

// Where somebody is in the world, as they tell the others: "ROOM X Y LEFT WALKING", like "lobby 250 640 1 0".
struct Place {
    std::string room {lobby};
    // Where their feet are, in hundredths of a tile from the room's top left corner.
    int x = 0;
    int y = 0;
    // Facing left (or else right), and walking (or else standing still).
    bool left = false;
    bool walking = false;

    bool operator==(const Place&) const = default;
};

std::string encode_place(const Place& place);

// Reads a place as encode_place() writes it; nullopt for anything else (a room whose name is not lowercase letters,
// digits and '-', a place outside of any room, flags that are not 0 or 1).
std::optional<Place> parse_place(std::string_view text);

// What a chat message shows in a bubble over its sender's head: its text without its tags (see markup::render()),
// the reply alone when it quotes another message, its spaces collapsed, and at most max_chars characters, with "…"
// at the end when it is cut. Empty when nothing is left to show.
std::string bubble_text(std::string_view message, std::size_t max_chars = 160);

} // namespace zchat::world
