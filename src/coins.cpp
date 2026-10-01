#include "coins.hpp"

#include <algorithm>
#include <charconv>

namespace zchat::coins {

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

} // namespace zchat::coins
