#include "game.hpp"

#include "casino.hpp"
#include "text.hpp"

#include <algorithm>
#include <array>
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

    // The games that are the casino's tables, by name.
    constexpr std::array<std::string_view, 3> casino_tables {"blackjack", "roulette", "horses"};

} // namespace

Games::Games(Chat& chat, Screen& terminal, std::function<void()> kicked) :
    chat_(chat),
    screen_(terminal) {
    games_.push_back(make_race(chat, terminal, *this));
    games_.push_back(make_dice(chat, terminal, *this));
    games_.push_back(make_paint(chat, terminal, *this));
    games_.push_back(make_wordle(chat, terminal, *this));
    games_.push_back(make_pokemon(chat, terminal, *this));
    games_.push_back(make_bomber(chat, terminal, *this));
    games_.push_back(make_arena(chat, terminal, *this));
    games_.push_back(make_blackjack(chat, terminal, *this));
    games_.push_back(make_roulette(chat, terminal, *this));
    games_.push_back(make_horses(chat, terminal, *this));
    // Kicks cost coins, which the ratings keep (made next).
    games_.push_back(make_kick(chat, terminal, std::move(kicked), [this](std::string_view item) {
        return ratings_->spend(item);
    }));
    // Spying is paid only when accepted: the asker pays, the one who agrees gets half. The reward is paid from the
    // screenshot thread, so these lock like the rest of Games.
    games_.push_back(make_spy(
        chat, terminal,
        [this](long long amount) {
            std::scoped_lock lock(mutex_);
            return ratings_->can_afford(amount);
        },
        [this](std::string_view item) {
            std::scoped_lock lock(mutex_);
            return ratings_->spend(item);
        },
        [this](long long amount) {
            std::scoped_lock lock(mutex_);
            ratings_->cash(amount);
        }));
    std::vector<std::string> rated;
    for (const auto& g : games_) {
        if (g->rated()) {
            rated.emplace_back(g->name());
        }
    }
    auto ratings = make_ratings(chat, terminal, std::move(rated));
    ratings_ = ratings.get();
    games_.push_back(std::move(ratings));

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
        .claims =
            [this](std::uint64_t sender, std::string_view text) {
                std::scoped_lock lock(mutex_);
                return std::ranges::any_of(games_, [&](const auto& g) {
                    return g->claims(sender, text);
                });
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
    if ((name == "leaderboard" || name == "leaderboards" || name == "top") && lowercase(args) == "coins") {
        ratings_->richest();
        return;
    }
    if (name == "leaderboard" || name == "leaderboards" || name == "top") {
        ratings_->leaderboard(lowercase(args));
        return;
    }
    if (name == "elo" || name == "rating" || name == "ratings") {
        ratings_->profile(args);
        return;
    }
    Game* game = find(name);
    if (!game) {
        chat_.game_notice(std::format("Unknown game {} (try /game)", text::sanitize(arg.substr(0, space), 32)));
        return;
    }
    if (args.empty()) {
        game->start();
    } else if (!game->command(game->keeps_case() ? std::string(args) : lowercase(args))) {
        chat_.game_notice(
            std::format("Unknown command {} for {} (try /game help {})", text::sanitize(args, 32), name, name));
    }
}

int Games::add_win(std::string_view game, std::uint64_t id, std::string_view name) {
    std::scoped_lock lock(mutex_);
    Score& score = scores_[lowercase(name)];
    score.name = chat_.colored_name(id, name);
    ++score.total;
    return ++score.wins[std::string(game)];
}

void Games::rate(std::string_view game, const std::vector<Placing>& placings) {
    std::scoped_lock lock(mutex_);
    ratings_->rate(game, placings);
}

void Games::kick(std::string_view args) {
    std::scoped_lock lock(mutex_);
    const auto it = std::ranges::find(games_, "kick", &Game::name);
    if (args.empty()) {
        (*it)->start();
    } else {
        (*it)->command(args);
    }
}

void Games::spy(std::string_view args) {
    std::scoped_lock lock(mutex_);
    const auto it = std::ranges::find(games_, "spy", &Game::name);
    if (args.empty()) {
        (*it)->start();
    } else {
        (*it)->command(args);
    }
}

void Games::coins(std::string_view args) {
    std::scoped_lock lock(mutex_);
    const std::string word = lowercase(args);
    if (word == "top" || word == "leaderboard") {
        ratings_->richest();
    } else if (word == "shop" || word == "prices") {
        ratings_->shop();
    } else {
        ratings_->wallet(args);
    }
}

bool Games::spend(std::string_view item) {
    std::scoped_lock lock(mutex_);
    return ratings_->spend(item);
}

void Games::casino(std::string_view args) {
    std::scoped_lock lock(mutex_);
    while (!args.empty() && args.front() == ' ') {
        args.remove_prefix(1);
    }
    const auto space = args.find(' ');
    const std::string name = lowercase(args.substr(0, space));
    std::string_view rest = space == std::string_view::npos ? std::string_view() : args.substr(space + 1);
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    if (name.empty() || name == "help") {
        if (name.empty() && screen_.show_game("casino", "{\"open\":true}")) {
            chat_.game_notice("🎰 The casino is open, in its own window: blackjack, roulette and the horse race, for "
                              "coins. /casino help: how to play in the chat.");
            return;
        }
        chat_.game_notice(
            "🎰 The casino: bet your coins at tables the whole chat shares. Whoever bets first at a table "
            "deals, and everybody sees everybody's bets.");
        for (const std::string_view t : casino_tables) {
            const Game* game = table(t);
            chat_.game_notice(std::format("  {}: {}", game->name(), game->summary()));
            for (const Help& h : game->help()) {
                chat_.game_notice(
                    std::format("    /casino {}{}{}  {}", game->name(), h.args.empty() ? "" : " ", h.args, h.what));
            }
        }
        chat_.game_notice(
            std::format("  Bets are from {} to {} coins. /coins: yours.", casino::min_bet, casino::max_bet));
        return;
    }
    Game* game = table(name);
    if (!game) {
        chat_.game_notice(
            std::format("No table {} at the casino: there are blackjack, roulette and horses (try /casino "
                        "help).",
                        text::sanitize(args.substr(0, space), 32)));
        return;
    }
    if (rest.empty()) {
        game->start();
    } else if (!game->command(lowercase(rest))) {
        chat_.game_notice(
            std::format("Unknown command {} at {} (try /casino help)", text::sanitize(rest, 32), game->name()));
    }
}

bool Games::stake(long long amount, std::string_view what) {
    std::scoped_lock lock(mutex_);
    return ratings_->stake(amount, what);
}

void Games::cash(long long amount) {
    std::scoped_lock lock(mutex_);
    ratings_->cash(amount);
}

Game* Games::table(std::string_view name) const {
    const std::string_view full = name == "bj" || name == "21" ? "blackjack"
                                  : name == "wheel"            ? "roulette"
                                  : name == "horse" || name == "race" || name == "horserace" ? "horses"
                                                                                             : name;
    if (std::ranges::find(casino_tables, full) == casino_tables.end()) {
        return nullptr;
    }
    const auto it = std::ranges::find(games_, full, &Game::name);
    return it == games_.end() ? nullptr : it->get();
}

Game* Games::find(std::string_view name) const {
    const auto it = std::ranges::find_if(games_, [&](const auto& g) {
        return g->listed() && g->name() == name;
    });
    return it == games_.end() ? nullptr : it->get();
}

void Games::list() const {
    chat_.game_notice("Games everyone in the chat can play:");
    for (const auto& g : games_) {
        if (!g->listed()) {
            continue;
        }
        chat_.game_notice(std::format("  {:<8} {}", g->name(), g->summary()));
    }
    chat_.game_notice("  /game NAME        start one, like /game race");
    chat_.game_notice("  /game help NAME   how to play one, and its commands");
    chat_.game_notice("  /game scores      who won what in this session");
    chat_.game_notice("  /game top [NAME]  the Elo leaderboard of all games, or of one (all: each of them); also "
                      "/game leaderboard");
    chat_.game_notice("  /game elo [NAME]  somebody's Elo ratings, yours without a name");
    chat_.game_notice(
        "  /game top coins   who has the most coins, won in the games (/coins: yours, /shop: what they buy)");
}

void Games::print_help(std::string_view name) const {
    if (name.empty()) {
        list();
        return;
    }
    const Game* found = find(name);
    if (!found) {
        chat_.game_notice(std::format("Unknown game {} (try /game)", text::sanitize(name, 32)));
        return;
    }
    const Game& game = *found;
    chat_.game_notice(std::format("{}: {}", game.name(), game.summary()));
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
        chat_.game_notice(std::format("  {:<{}}  {}", usages[i], width, help[i].what));
    }
}

void Games::print_scores() const {
    if (scores_.empty()) {
        chat_.game_notice("Nobody has won a game yet: /game lists them.");
        return;
    }
    std::vector<const Score*> ranking;
    for (const auto& [key, score] : scores_) {
        ranking.push_back(&score);
    }
    std::ranges::stable_sort(ranking, std::ranges::greater {}, &Score::total);
    chat_.game_notice("Wins in this session:");
    for (const Score* score : ranking) {
        std::string games;
        for (const auto& [game, wins] : score->wins) {
            games += std::format("{}{} {}", games.empty() ? "" : ", ", game, wins);
        }
        chat_.game_notice(std::format("  {} {} ({})", score->total, score->name, games));
    }
}

} // namespace zchat::game
