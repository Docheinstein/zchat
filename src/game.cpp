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
    games_.push_back(make_wordle(chat, terminal, *this));
    games_.push_back(make_pokemon(chat, terminal, *this));

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
    if (name == "help") {
        print_help(lowercase(args));
        return;
    }
    const auto it = std::ranges::find(games_, name, &Game::name);
    if (it == games_.end()) {
        chat_.notice(std::format("Unknown game {} (try /game)", text::sanitize(arg.substr(0, space), 32)));
        return;
    }
    if (args.empty()) {
        (*it)->start();
    } else if (!(*it)->command((*it)->keeps_case() ? std::string(args) : lowercase(args))) {
        chat_.notice(std::format("Unknown command {} for {} (try /game help {})", text::sanitize(args, 32), name,
                                 name));
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
    chat_.notice("Games everyone in the chat can play:");
    for (const auto& g : games_) {
        chat_.notice(std::format("  {:<8} {}", g->name(), g->summary()));
    }
    chat_.notice("  /game NAME        start one, like /game race");
    chat_.notice("  /game help NAME   how to play one, and its commands");
    chat_.notice("  /game scores      who won what in this session");
}

void Games::print_help(std::string_view name) const {
    if (name.empty()) {
        list();
        return;
    }
    const auto it = std::ranges::find(games_, name, &Game::name);
    if (it == games_.end()) {
        chat_.notice(std::format("Unknown game {} (try /game)", text::sanitize(name, 32)));
        return;
    }
    const Game& game = **it;
    chat_.notice(std::format("{}: {}", game.name(), game.summary()));
    auto help = game.help();
    if (help.empty()) {
        help.push_back({"", "start a round"});
    }
    // The commands in a column, after the longest that is not too long.
    std::vector<std::string> usages;
    std::size_t width = 0;
    for (const Help& h : help) {
        usages.push_back(h.args.empty() ? std::format("/game {}", game.name())
                                        : std::format("/game {} {}", game.name(), h.args));
        if (usages.back().size() <= 28) {
            width = std::max(width, usages.back().size());
        }
    }
    for (std::size_t i = 0; i < help.size(); ++i) {
        chat_.notice(std::format("  {:<{}}  {}", usages[i], width, help[i].what));
    }
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
