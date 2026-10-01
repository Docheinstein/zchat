#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Elo ratings for the games played in the chat (see game::Ratings): the arithmetic, and how ratings are written.
namespace zchat::elo {

// Everybody's first rating, in every game.
inline constexpr double initial = 1500;
// The name of the rating that every game counts towards.
inline constexpr std::string_view general = "general";
// Ratings of fewer games than this are still settling: they move faster, and are marked as such.
inline constexpr int provisional_games = 10;

struct Rating {
    double rating = initial;
    int games = 0;
};

// The ratings of somebody, by game ("general" for the one of all games).
using Ratings = std::map<std::string, Rating, std::less<>>;

// How much a result moves a rating at most: more while it is provisional.
double k_factor(const Rating& rating);

// The chance a player of rating a has to beat one of rating b, from 0 to 1.
double expected(double a, double b);

// How much each player's rating changes with the result of a round. places[i] is where player i finished, 0 for
// the first, and players with the same place tied. Each pair of players is a game of its own, won by the better
// placed (half to each for a tie), and with more opponents each one counts less: the K of the player is shared
// among them. Fewer than two players change nothing.
std::vector<double> deltas(const std::vector<Rating>& ratings, const std::vector<int>& places);

// Ratings as text, "GAME RATING GAMES" for each, separated by spaces: "general 1512.4 12 race 1530 5".
std::string encode(const Ratings& ratings);
// The ratings of text from encode(); nullopt when it is not well-formed. Game names are lowercase letters and
// digits, of at most max_game_name characters; ratings and games out of range are refused.
std::optional<Ratings> decode(std::string_view text);
inline constexpr std::size_t max_game_name = 16;
inline constexpr std::size_t max_games = 32;

} // namespace zchat::elo
