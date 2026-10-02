#pragma once

// What the casino's tables (blackjack.cpp, roulette.cpp, horses.cpp) have in common: a table is one for the whole chat,
// whoever bets first when it is free deals that round (shuffles, spins, starts the race: the referee), and each zchat
// takes its user's bets and pays them their winnings itself (see Games::stake() and Games::cash()), from what the
// dealer says happened. A round whose dealer goes quiet is given up, and the bets on it are given back.

#include "casino.hpp"
#include "color.hpp"
#include "game.hpp"

#include <charconv>
#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <random>
#include <string>

namespace zchat::game {

class CasinoTable : public Game {
public:
    bool listed() const override {
        return false;
    }

    bool rated() const override {
        return false;
    }

    void message(std::uint64_t, std::string_view, std::string_view) override {
    }

protected:
    using clock = std::chrono::steady_clock;

    CasinoTable(Chat& chat, Screen& terminal, Games& games) :
        chat_(chat),
        terminal_(terminal),
        games_(games),
        rng_(std::random_device {}()) {
    }

    // A new round's number: random, never 0.
    std::uint64_t new_round() {
        std::uint64_t round = 0;
        while (round == 0) {
            round = rng_();
        }
        return round;
    }

    // Somebody's name, as last heard: from their packets, or the chat's people.
    std::string name_of(std::uint64_t id) const {
        if (id == chat_.id()) {
            return chat_.name();
        }
        if (const auto it = names_.find(id); it != names_.end()) {
            return it->second;
        }
        for (const auto& [peer, name] : chat_.people()) {
            if (peer == id) {
                return name;
            }
        }
        return "Somebody";
    }

    std::string colored(std::uint64_t id) const {
        return id == chat_.id() ? chat_.colored_own_name() : chat_.colored_name(id, name_of(id));
    }

    // "you" for us, or else their colored name.
    std::string who(std::uint64_t id, bool capital = false) const {
        return id == chat_.id() ? (capital ? "You" : "you") : colored(id);
    }

    void heard(std::uint64_t id, std::string_view name) {
        if (!name.empty() && id != chat_.id()) {
            names_[id] = std::string(name);
        }
    }

    // A player, for the window: their name and color, and whether it is us.
    std::string player_json(std::uint64_t id) const {
        const Color c = color_of_id(id);
        return std::format("\"name\":{},\"color\":\"#{:02x}{:02x}{:02x}\",\"you\":{}", json(name_of(id)), c.r, c.g, c.b,
                           id == chat_.id());
    }

    // An amount of coins as typed, within the bets' limits; nullopt, saying why, when it is not one.
    std::optional<long long> amount(std::string_view typed) const {
        const auto n = parse_number(typed);
        if (!n || *n < static_cast<std::uint64_t>(casino::min_bet) || *n > static_cast<std::uint64_t>(casino::max_bet)) {
            chat_.notice(std::format("A bet is from {} to {} coins, like 10.", casino::min_bet, casino::max_bet));
            return std::nullopt;
        }
        return static_cast<long long>(*n);
    }

    // The whole seconds left until then, rounded up; 0 once it is past.
    static long long seconds_left(clock::time_point until) {
        const auto now = clock::now();
        return until <= now ? 0 : std::chrono::ceil<std::chrono::seconds>(until - now).count();
    }

    // Sends a packet of this table: its name, then event.
    void send(std::string_view event) {
        chat_.send_game(std::format("{} {}", name(), event));
    }

    static std::optional<std::uint64_t> parse_number(std::string_view s, int base = 10) {
        std::uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size()) {
            return std::nullopt;
        }
        return value;
    }

    // Takes the next word of s, up to a space, and removes it with the space.
    static std::string_view next_field(std::string_view& s) {
        const auto space = s.find(' ');
        const std::string_view field = s.substr(0, space);
        s.remove_prefix(space == std::string_view::npos ? s.size() : space + 1);
        return field;
    }

    static std::string plural(long long n, std::string_view word) {
        return std::format("{} {}{}", n, word, n == 1 || n == -1 ? "" : "s");
    }

    // A win or a loss of coins, as told of us ("win 15", "lose 10", "get your 10 back") or of somebody else ("wins
    // 15"...).
    static std::string net_text(long long net, long long staked, bool us = true) {
        return net > 0   ? std::format("win{} {}", us ? "" : "s", plural(net, "coin"))
               : net < 0 ? std::format("lose{} {}", us ? "" : "s", plural(-net, "coin"))
                         : std::format("get{} {} {} back", us ? "" : "s", us ? "your" : "their", plural(staked, "coin"));
    }

    static std::string json(std::string_view s) {
        std::string out = "\"";
        for (const char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
        return out + "\"";
    }

    Chat& chat_;
    Screen& terminal_;
    Games& games_;
    std::mt19937_64 rng_;
    // Names of the players, by sender id, as heard.
    std::map<std::uint64_t, std::string> names_;
};

} // namespace zchat::game
