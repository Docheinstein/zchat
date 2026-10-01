#include "elo.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace zchat::elo {

namespace {

    // Takes the next word of s, up to a space, and removes it with the spaces after it.
    std::string_view next_field(std::string_view& s) {
        const auto space = s.find(' ');
        const std::string_view field = s.substr(0, space);
        s.remove_prefix(space == std::string_view::npos ? s.size() : space);
        while (!s.empty() && s.front() == ' ') {
            s.remove_prefix(1);
        }
        return field;
    }

    bool valid_game(std::string_view name) {
        if (name.empty() || name.size() > max_game_name) {
            return false;
        }
        for (const char c : name) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
                return false;
            }
        }
        return true;
    }

    // A number like "1512" or "1512.4", by hand: from_chars() of a double is missing from some standard libraries.
    std::optional<double> parse_rating(std::string_view s) {
        const auto dot = s.find('.');
        const std::string_view whole = s.substr(0, dot);
        const std::string_view tenths = dot == std::string_view::npos ? std::string_view() : s.substr(dot + 1);
        int value = 0;
        const auto [ptr, ec] = std::from_chars(whole.data(), whole.data() + whole.size(), value);
        if (whole.empty() || whole.size() > 4 || ec != std::errc {} || ptr != whole.data() + whole.size() ||
            whole.front() == '-' ||
            (dot != std::string_view::npos && (tenths.size() != 1 || tenths[0] < '0' || tenths[0] > '9'))) {
            return std::nullopt;
        }
        return value + (tenths.empty() ? 0 : (tenths[0] - '0') / 10.0);
    }

} // namespace

double k_factor(const Rating& rating) {
    return rating.games < provisional_games ? 40 : 20;
}

double expected(double a, double b) {
    return 1 / (1 + std::pow(10.0, (b - a) / 400));
}

std::vector<double> deltas(const std::vector<Rating>& ratings, const std::vector<int>& places) {
    std::vector<double> out(ratings.size(), 0.0);
    const std::size_t n = std::min(ratings.size(), places.size());
    if (n < 2) {
        return out;
    }
    for (std::size_t i = 0; i < n; ++i) {
        double score = 0;
        double expect = 0;
        for (std::size_t j = 0; j < n; ++j) {
            if (i == j) {
                continue;
            }
            score += places[i] < places[j] ? 1.0 : places[i] == places[j] ? 0.5 : 0.0;
            expect += expected(ratings[i].rating, ratings[j].rating);
        }
        out[i] = k_factor(ratings[i]) * (score - expect) / static_cast<double>(n - 1);
    }
    return out;
}

std::string encode(const Ratings& ratings) {
    std::string out;
    for (const auto& [game, r] : ratings) {
        // In tenths, never below 0 nor above what decode() takes.
        const double rating = std::clamp(std::round(r.rating * 10) / 10, 0.0, 5000.0);
        out += std::format("{}{} {:.1f} {}", out.empty() ? "" : " ", game, rating, r.games);
    }
    return out;
}

std::optional<Ratings> decode(std::string_view text) {
    Ratings out;
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty()) {
        const std::string_view game = next_field(text);
        const std::string_view rating = next_field(text);
        const std::string_view games = next_field(text);
        Rating r;
        const auto value = parse_rating(rating);
        const auto [gp, gec] = std::from_chars(games.data(), games.data() + games.size(), r.games);
        if (!valid_game(game) || !value || *value > 5000 || games.empty() || gec != std::errc {} ||
            gp != games.data() + games.size() || r.games < 0 || r.games > 10'000'000 || out.size() >= max_games) {
            return std::nullopt;
        }
        r.rating = *value;
        out[std::string(game)] = r;
    }
    return out;
}

} // namespace zchat::elo
