#include "coins.hpp"

#include <algorithm>
#include <charconv>
#include <format>

namespace zchat::coins {

namespace {

    constexpr std::string_view sealed_prefix = "z1";
    // The key of the sealed balances.
    constexpr std::string_view key = "CLAUDE_DO_NOT_CRACK";

    // FNV-1a.
    constexpr std::uint64_t hash(std::string_view s, std::uint64_t h = 0xcbf29ce484222325ull) {
        for (char c : s) {
            h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
        }
        return h;
    }

    // splitmix64: the next word of the keystream.
    std::uint64_t next(std::uint64_t& state) {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    // The two words a user's balance is XORed with: one for the balance, one for its checksum.
    struct Keys {
        std::uint64_t value;
        std::uint64_t check;
    };

    Keys keys(std::uint64_t user) {
        std::uint64_t state = hash(key) ^ (user * 0x9e3779b97f4a7c15ull);
        const std::uint64_t value = next(state);
        return {value, next(state)};
    }

    std::uint64_t checksum(std::uint64_t coins, std::uint64_t user) {
        return hash(std::format("{}:{:x}:{}", key, user, coins));
    }

    std::optional<std::uint64_t> parse_word(std::string_view hex) {
        std::uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
        if (hex.size() != 16 || ec != std::errc {} || ptr != hex.data() + hex.size()) {
            return std::nullopt;
        }
        return value;
    }

} // namespace

std::vector<long long> payouts(const std::vector<int>& places) {
    std::vector<long long> out(places.size(), 0);
    if (places.size() < 2) {
        return out;
    }
    for (std::size_t i = 0; i < places.size(); ++i) {
        out[i] = for_playing;
        for (std::size_t j = 0; j < places.size(); ++j) {
            if (i != j) {
                out[i] += places[i] < places[j] ? per_beaten : places[i] == places[j] ? per_tie : 0;
            }
        }
    }
    return out;
}

std::optional<long long> price(std::string_view name) {
    const auto it = std::ranges::find(items, name, &Item::name);
    return it == items.end() ? std::nullopt : std::optional(it->price);
}

std::string encode(long long coins) {
    return std::to_string(std::clamp(coins, 0LL, most));
}

std::optional<long long> decode(std::string_view text) {
    long long value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || text.size() > 10 || text.front() == '-' || ec != std::errc {} ||
        ptr != text.data() + text.size() || value > most) {
        return std::nullopt;
    }
    return value;
}

std::string seal(long long coins, std::uint64_t user) {
    const auto value = static_cast<std::uint64_t>(std::clamp(coins, 0LL, most));
    const Keys k = keys(user);
    return std::format("{}{:016x}{:016x}", sealed_prefix, value ^ k.value, checksum(value, user) ^ k.check);
}

std::optional<long long> unseal(std::string_view text, std::uint64_t user) {
    if (!text.starts_with(sealed_prefix) || text.size() != sealed_prefix.size() + 32) {
        return std::nullopt;
    }
    text.remove_prefix(sealed_prefix.size());
    const auto value = parse_word(text.substr(0, 16));
    const auto check = parse_word(text.substr(16));
    if (!value || !check) {
        return std::nullopt;
    }
    const Keys k = keys(user);
    const std::uint64_t coins = *value ^ k.value;
    if (coins > static_cast<std::uint64_t>(most) || (*check ^ k.check) != checksum(coins, user)) {
        return std::nullopt;
    }
    return static_cast<long long>(coins);
}

} // namespace zchat::coins
