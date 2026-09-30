#include "game.hpp"

#include "text.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace zchat::game {

namespace {

    std::string lowercase(std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        return out;
    }

} // namespace

Games::Games(Chat& chat, Screen& terminal) :
    chat_(chat) {
    games_.push_back(make_race(chat, terminal, *this));
    games_.push_back(make_dice(chat, terminal, *this));
    games_.push_back(make_paint(chat, terminal, *this));

    chat_.set_game_hooks({
        .packet =
            [this](std::uint64_t sender, std::string_view name, std::string_view text) {
                const auto space = text.find(' ');
                const std::string_view game = text.substr(0, space);
                std::scoped_lock lock(mutex_);
                // A game this version does not have, from a newer one, is ignored.
                const auto it = std::ranges::find(games_, game, &Game::name);
                if (it != games_.end()) {
                    (*it)->receive(sender, name,
                                   space == std::string_view::npos ? std::string_view() : text.substr(space + 1));
                }
            },
        .message =
            [this](std::uint64_t sender, std::string_view name, std::string_view text) {
                std::scoped_lock lock(mutex_);
                for (const auto& g : games_) {
                    g->message(sender, name, text);
                }
            },
        .tick =
            [this] {
                std::scoped_lock lock(mutex_);
                for (const auto& g : games_) {
                    g->tick();
                }
            },
    });
}

Games::~Games() {
    // The chat outlives us: it must stop calling into games that are gone.
    chat_.set_game_hooks({});
}

void Games::command(std::string_view arg) {
    std::scoped_lock lock(mutex_);
    if (arg.empty()) {
        list();
        return;
    }
    const auto space = arg.find(' ');
    const std::string name = lowercase(arg.substr(0, space));
    std::string_view args = space == std::string_view::npos ? std::string_view() : arg.substr(space + 1);
    while (!args.empty() && args.front() == ' ') {
        args.remove_prefix(1);
    }
    if (name == "scores" && args.empty()) {
        print_scores();
        return;
    }
    const auto it = std::ranges::find(games_, name, &Game::name);
    if (it == games_.end()) {
        chat_.notice(std::format("Unknown game {} (try /game)", text::sanitize(arg.substr(0, space), 32)));
        return;
    }
    if (args.empty()) {
        (*it)->start();
    } else if (!(*it)->command(lowercase(args))) {
        const std::string_view commands = (*it)->commands();
        chat_.notice(commands.empty() ? std::format("{} has no commands: /game {} starts a round.", name, name)
                                      : std::format("Unknown command {} for {} (its commands: {})",
                                                    text::sanitize(args, 32), name, commands));
    }
}

int Games::add_win(std::string_view game, std::uint64_t id, std::string_view name) {
    std::scoped_lock lock(mutex_);
    Score& score = scores_[lowercase(name)];
    score.name = chat_.colored_name(id, name);
    ++score.total;
    return ++score.wins[std::string(game)];
}

void Games::list() const {
    chat_.notice("Games everyone in the chat can play: /game NAME starts one.");
    for (const auto& g : games_) {
        chat_.notice(std::format("  {:<8} {}", g->name(), g->summary()));
        if (!g->commands().empty()) {
            // "roll, stop" becomes "/game dice roll, /game dice stop".
            std::string line;
            for (std::string_view rest = g->commands(); !rest.empty();) {
                const auto comma = rest.find(", ");
                line += std::format("{}/game {} {}", line.empty() ? "" : ", ", g->name(), rest.substr(0, comma));
                rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 2);
            }
            chat_.notice(std::format("           then {}", line));
        }
    }
    chat_.notice("  /game scores  who won what in this session");
}

void Games::print_scores() const {
    if (scores_.empty()) {
        chat_.notice("Nobody has won a game yet: /game lists them.");
        return;
    }
    std::vector<const Score*> ranking;
    for (const auto& [key, score] : scores_) {
        ranking.push_back(&score);
    }
    std::ranges::stable_sort(ranking, std::ranges::greater {}, &Score::total);
    chat_.notice("Wins in this session:");
    for (const Score* score : ranking) {
        std::string games;
        for (const auto& [game, wins] : score->wins) {
            games += std::format("{}{} {}", games.empty() ? "" : ", ", game, wins);
        }
        chat_.notice(std::format("  {} {} ({})", score->total, score->name, games));
    }
}

} // namespace zchat::game
