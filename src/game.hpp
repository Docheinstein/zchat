#pragma once

#include "chat.hpp"
#include "screen.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace zchat::game {

// A command of a game, for /game help NAME: /game NAME args, and what it does.
struct Help {
    std::string_view args;
    std::string_view what;
};

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
    // Whether a chat line of another peer is a move of this game (a race's words), which a window shows in the games
    // log rather than in #general (see Chat::GameHooks::claims).
    virtual bool claims(std::uint64_t sender, std::string_view text) const {
        (void)sender;
        (void)text;
        return false;
    }
    // /game NAME ARGS, like /game dice roll: returns whether the game has such a command.
    virtual bool command(std::string_view args) {
        (void)args;
        return false;
    }
    // Whether command() gets its args as typed; or else lowercase, as most games want them.
    virtual bool keeps_case() const {
        return false;
    }
    // Its commands, for /game help NAME: what follows /game NAME, like "roll", and what it does. Empty when /game
    // NAME is all there is.
    virtual std::vector<Help> help() const {
        return {};
    }
    // Whether it is one of the games of /game; or else it only rides on their packets, with a command of its own (see
    // /kick).
    virtual bool listed() const {
        return true;
    }
    // Whether its rounds have winners, and so Elo ratings (see Ratings).
    virtual bool rated() const {
        return listed();
    }
};

// Where a player finished a round of a game: place 0 is the first, and players with the same place tied.
struct Placing {
    std::uint64_t id = 0; // sender id
    std::string name;
    int place = 0;
};

// The Elo ratings of the games, one per game and a general one of them all, and their leaderboards (see
// ratings.cpp). There is no server to keep them: each zchat keeps its user's own, in the config, works them out from
// the results it sees, and tells the others, who keep what they hear for the leaderboards (also of who left).
class Ratings : public Game {
public:
    // A round of a game ended, with these players where they finished: rates them, and shows the new ratings.
    virtual void rate(std::string_view game, const std::vector<Placing>& placings) = 0;
    // /game leaderboard [GAME|all]: the general leaderboard, a game's, or all of them.
    virtual void leaderboard(std::string_view game) = 0;
    // /game elo [NAME]: somebody's ratings (ours without a name).
    virtual void profile(std::string_view name) = 0;

    // The coins won in the rounds (see coins.hpp), kept and told around like the ratings.
    // /coins [NAME]: somebody's coins (ours without a name).
    virtual void wallet(std::string_view name) = 0;
    // /coins top: who has the most.
    virtual void richest() = 0;
    // /shop: what coins buy, and how to win them.
    virtual void shop() = 0;
    // Pays for one of coins::items, by name: returns false, saying why, when we cannot afford it.
    virtual bool spend(std::string_view item) = 0;
    // Whether we have at least this many coins (to check before promising to pay, without spending yet).
    virtual bool can_afford(long long amount) const = 0;
    // A bet at the casino (see casino.hpp): takes amount coins, or returns false, saying why, when we do not have
    // them. what is where, as in "blackjack".
    virtual bool stake(long long amount, std::string_view what) = 0;
    // What the casino gives back: our winnings, or a bet back.
    virtual void cash(long long amount) = 0;
};

// The games that can be played with /game NAME. All calls are safe from any thread.
class Games {
public:
    // kicked is called when the others voted us out of the chat (see /kick): zchat must quit.
    Games(Chat& chat, Screen& terminal, std::function<void()> kicked = {});
    ~Games();
    Games(const Games&) = delete;
    Games& operator=(const Games&) = delete;

    // /game ARG: without a game's name, lists them; "scores" shows who won what, "help NAME" how to play a game,
    // "leaderboard [GAME]" the Elo leaderboards and "elo [NAME]" somebody's ratings.
    // /game NAME starts a round of a game, and /game NAME ARGS is one of its own commands, like /game dice roll.
    void command(std::string_view arg);

    // Counts a win of a round of a game; returns how many that player has won in this session. Every zchat
    // counts the wins it sees.
    int add_win(std::string_view game, std::uint64_t id, std::string_view name);

    // A round of a game ended, with these players where they finished: changes their Elo ratings (see Ratings).
    // Rounds of fewer than two players are not rated.
    void rate(std::string_view game, const std::vector<Placing>& placings);

    // /kick ARGS: asks the others to vote someone out of the chat, or votes (see kick.cpp).
    void kick(std::string_view args);

    // /spy ARGS: asks someone to let us see their screen(s), or answers a request about us (see spy.cpp).
    void spy(std::string_view args);

    // /coins ARGS: ours, somebody's (NAME), who has the most (top), or what they buy (shop).
    void coins(std::string_view args);
    // Pays for an annoying command, one of coins::items: returns false, saying why, when we cannot afford it.
    bool spend(std::string_view item);

    // /casino ARGS: without any, the casino's tables (and its window opens, where there is one); "help" how to play;
    // or a table's name (blackjack, roulette, horses), alone to show it, or with one of its commands.
    void casino(std::string_view args);
    // Bets at the casino, and what it pays back (see Ratings::stake() and Ratings::cash()).
    bool stake(long long amount, std::string_view what);
    void cash(long long amount);

private:
    struct Score {
        std::string name;                             // colored, as last seen
        std::map<std::string, int, std::less<>> wins; // by game
        int total = 0;
    };

    // A game of /game, by lowercase name; nullptr for none.
    Game* find(std::string_view name) const;
    // A table of the casino, by lowercase name or nickname (bj); nullptr for none.
    Game* table(std::string_view name) const;
    void list() const;
    void print_help(std::string_view name) const;
    void print_scores() const;

    Chat& chat_;
    Screen& screen_;
    // Recursive, as games count their wins while one of their calls holds it.
    mutable std::recursive_mutex mutex_;
    std::vector<std::unique_ptr<Game>> games_;
    // One of games_.
    Ratings* ratings_ = nullptr;
    // By lowercase name: the id changes with the color.
    std::map<std::string, Score> scores_;
};

// The games, each in its own file.
std::unique_ptr<Game> make_race(Chat& chat, Screen& terminal, Games& games);
std::unique_ptr<Game> make_dice(Chat& chat, Screen& terminal, Games& games);
std::unique_ptr<Game> make_paint(Chat& chat, Screen& terminal, Games& games);
std::unique_ptr<Game> make_wordle(Chat& chat, Screen& terminal, Games& games);
std::unique_ptr<Game> make_candor(Chat& chat, Screen& terminal, Games& games);
std::unique_ptr<Game> make_pokemon(Chat& chat, Screen& terminal, Games& games);
std::unique_ptr<Game> make_bomber(Chat& chat, Screen& terminal, Games& games);
std::unique_ptr<Game> make_arena(Chat& chat, Screen& terminal, Games& games);
std::unique_ptr<Game> make_frag(Chat& chat, Screen& terminal, Games& games);
// Not a game: the Elo ratings and the coins, told around as Game packets; rated_games are the names of the games
// they are for.
std::unique_ptr<Ratings> make_ratings(Chat& chat, Screen& terminal, std::vector<std::string> rated_games);
// The tables of the casino (/casino), not games of /game: played for coins, and not rated.
std::unique_ptr<Game> make_blackjack(Chat& chat, Screen& terminal, Games& games);
std::unique_ptr<Game> make_roulette(Chat& chat, Screen& terminal, Games& games);
std::unique_ptr<Game> make_horses(Chat& chat, Screen& terminal, Games& games);
// Not a game: the votes of /kick, which travel as Game packets too; spend pays for one (see Games::spend()).
std::unique_ptr<Game> make_kick(Chat& chat, Screen& terminal, std::function<void()> kicked,
                                std::function<bool(std::string_view)> spend);
// Not a game: /spy, which rides on Game packets too. Nobody pays unless the peek is accepted: can_afford checks the
// asker has the coins before asking, spend takes them once it is accepted, and cash gives the agreeing person their
// half. See spy.cpp.
std::unique_ptr<Game> make_spy(Chat& chat, Screen& terminal, std::function<bool(long long)> can_afford,
                               std::function<bool(std::string_view)> spend, std::function<void(long long)> cash);

} // namespace zchat::game
