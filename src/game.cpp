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

Games::Games(Chat& chat, Terminal& terminal) :
    chat_(chat) {
    games_.push_back(make_finger(chat, terminal, *this));

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
    if (lowercase(arg) == "scores") {
        print_scores();
        return;
    }
    const auto it = std::ranges::find_if(games_, [&](const auto& g) {
        return lowercase(arg) == g->name();
    });
    if (it == games_.end()) {
        chat_.notice(std::format("Unknown game {} (try /game)", text::sanitize(arg, 32)));
        return;
    }
    (*it)->start();
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
