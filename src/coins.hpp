#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Coins: the virtual money won in the games played in the chat and spent on the annoying commands, like /trill and
// /kick (see game::Ratings, which keeps them): the arithmetic, the prices, and how a balance is written.
namespace zchat::coins {

// Everybody's coins before their first game: enough to trill and kick a few times right away.
inline constexpr long long initial = 50;
// The most coins anybody can have.
inline constexpr long long most = 1'000'000'000;

// What a round pays each of its players: something for playing, and more for each player they beat (half of it for
// each they tied with), so the winner of a round of many gets the most.
inline constexpr long long for_playing = 2;
inline constexpr long long per_beaten = 4;
inline constexpr long long per_tie = per_beaten / 2;

// The coins each player of a round wins. places[i] is where player i finished, 0 for the first, and players with the
// same place tied. Fewer than two players win nothing.
std::vector<long long> payouts(const std::vector<int>& places);

// Something coins buy: what it is called (as spend() takes it), its price, and what it is, for /shop. A thing that is
// not built yet is listed as coming soon.
struct Item {
    std::string_view name;
    long long price;
    std::string_view what;
    bool soon = false;
};

inline constexpr std::array items {
    Item {"trill", 5, "/trill: Fahhh at everybody else"},
    Item {"custom trill", 8, "/trill SOUND PICTURE: your own sound, picture, or both"},
    Item {"kick", 20, "/kick NAME: ask the chat to vote someone out (paid when the vote starts)"},
    Item {"spy", 10, "/spy @NAME: peek at their screen(s) — only if they accept (then they get half)"},
};

// The price of a thing by name; nullopt for none.
std::optional<long long> price(std::string_view name);

// A balance as text, and back: a whole number from 0 to most; nullopt when it is not one.
std::string encode(long long coins);
std::optional<long long> decode(std::string_view text);

// A balance as kept on disk, ours in the config and the others' in the "coins" file, so that it is not a number to
// change in a text editor: encrypted, with a checksum, for one user (their id, see Chat::user()), so that another's
// cannot be copied over ours either. "z1" and 32 hex digits. It is a simple cipher whose key is in this source, so
// it stops the casual edit, not someone who reads the code.
std::string seal(long long coins, std::uint64_t user);
// nullopt when it is not a sealed balance of this user, or was changed by hand.
std::optional<long long> unseal(std::string_view text, std::uint64_t user);

} // namespace zchat::coins
