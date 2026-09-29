#pragma once

#include "chat.hpp"
#include "terminal.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace zchat::game {

// A mini-game everyone in the chat plays together. There is no server: whoever starts a round is its referee,
// and sends everybody what happens in it (as Game packets whose text starts with the game's name).
class Game {
public:
    virtual ~Game() = default;

    // The word after /game.
    virtual std::string_view name() const = 0;
    // What it is, for the list of games.
    virtual std::string_view summary() const = 0;

    // /game NAME: starts a round, with us as its referee.
    virtual void start() = 0;
    // A Game packet for this game from another peer: its text after the game's name and a space.
    virtual void receive(std::uint64_t sender, std::string_view name, std::string_view text) = 0;
    // A chat line, ours included.
    virtual void message(std::uint64_t sender, std::string_view name, std::string_view text) = 0;
    // Called a few times per second, for the timers.
    virtual void tick() = 0;
    // /game NAME ARGS, like /game dice roll: returns whether the game has such a command.
    virtual bool command(std::string_view args) {
        (void)args;
        return false;
    }
    // Its commands, like "roll, stop", for the list of games; empty when it has none.
    virtual std::string_view commands() const {
        return {};
    }
};

// The games that can be played with /game NAME. All calls are safe from any thread.
class Games {
public:
    Games(Chat& chat, Terminal& terminal);
    ~Games();
    Games(const Games&) = delete;
    Games& operator=(const Games&) = delete;

    // /game ARG: without a game's name, lists them; "scores" shows who won what. /game NAME starts a round of a
    // game, and /game NAME ARGS is one of its own commands, like /game dice roll.
    void command(std::string_view arg);

    // Counts a win of a round of a game; returns how many that player has won in this session. Every zchat
    // counts the wins it sees.
    int add_win(std::string_view game, std::uint64_t id, std::string_view name);

private:
    struct Score {
        std::string name;                             // colored, as last seen
        std::map<std::string, int, std::less<>> wins; // by game
        int total = 0;
    };

    void list() const;
    void print_scores() const;

    Chat& chat_;
    // Recursive, as games count their wins while one of their calls holds it.
    mutable std::recursive_mutex mutex_;
    std::vector<std::unique_ptr<Game>> games_;
    // By lowercase name: the id changes with the color.
    std::map<std::string, Score> scores_;
};

// The games, each in its own file.
std::unique_ptr<Game> make_race(Chat& chat, Terminal& terminal, Games& games);
std::unique_ptr<Game> make_dice(Chat& chat, Terminal& terminal, Games& games);
std::unique_ptr<Game> make_paint(Chat& chat, Terminal& terminal, Games& games);

} // namespace zchat::game
