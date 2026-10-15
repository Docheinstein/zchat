#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace zchat::emojis {

// The Unicode emoji, by name: GitHub's gemoji list (third_party/gemoji), built into zchat. They are typed as
// :shortcodes: (":rofl:" becomes 🤣 when sent, see replace()), picked in the window's emoji picker, and used for
// reactions to messages. (The emoji saved with /addemoji are pictures of one's own: see /emoji.)

struct Emoji {
    std::string emoji;
    std::string description;
    std::string category;
    // The names it is typed with, between colons ("rofl"), the first being its main one.
    std::vector<std::string> aliases;
    // Other words it is found by ("laugh").
    std::vector<std::string> tags;
};

// The emoji of a gemoji emoji.json, in its order (by category); the ones it cannot read are left out.
std::vector<Emoji> parse(std::string_view json);

// All of them, from the list built into zchat (parsed once). Thread-safe.
const std::vector<Emoji>& all();

// An emoji by one of its names (without the colons), or by itself; nullptr when there is none.
const Emoji* find(std::string_view name, const std::vector<Emoji>& table = all());

// Text with its :shortcodes: replaced by their emoji (":rofl:" becomes 🤣); colons around anything else stay.
std::string replace(std::string_view text, const std::vector<Emoji>& table = all());

// The emoji whose names start with what is typed (best), or contain it, or whose tags or description do, up to max.
std::vector<const Emoji*> search(std::string_view typed, std::size_t max, const std::vector<Emoji>& table = all());

} // namespace zchat::emojis
