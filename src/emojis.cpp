#include "emojis.hpp"

#include "json.hpp"

#include <algorithm>

namespace zchat::emojis {

namespace {

    bool name_char(char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-';
    }

    std::string lower(std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](char c) {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        });
        return out;
    }

} // namespace

std::vector<Emoji> parse(std::string_view text) {
    std::vector<Emoji> out;
    const auto list = json::parse(text);
    if (!list || !list->is_array()) {
        return out;
    }
    for (const auto& item : list->items) {
        Emoji e;
        e.emoji = std::string(item["emoji"].str());
        e.description = std::string(item["description"].str());
        e.category = std::string(item["category"].str());
        for (const auto& alias : item["aliases"].items) {
            if (!alias.str().empty()) {
                e.aliases.emplace_back(alias.str());
            }
        }
        for (const auto& tag : item["tags"].items) {
            if (!tag.str().empty()) {
                e.tags.emplace_back(tag.str());
            }
        }
        if (!e.emoji.empty() && !e.aliases.empty()) {
            out.push_back(std::move(e));
        }
    }
    return out;
}

const Emoji* find(std::string_view name, const std::vector<Emoji>& table) {
    for (const auto& e : table) {
        if (e.emoji == name || std::ranges::find(e.aliases, name) != e.aliases.end()) {
            return &e;
        }
    }
    return nullptr;
}

std::string replace(std::string_view text, const std::vector<Emoji>& table) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto colon = text.find(':', i);
        if (colon == std::string_view::npos) {
            break;
        }
        out += text.substr(i, colon - i);
        std::size_t end = colon + 1;
        while (end < text.size() && end - colon <= 64 && name_char(text[end])) {
            ++end;
        }
        if (end < text.size() && text[end] == ':' && end > colon + 1) {
            if (const Emoji* e = find(text.substr(colon + 1, end - colon - 1), table)) {
                out += e->emoji;
                i = end + 1;
                continue;
            }
        }
        // Not a shortcode: the colon stays, and the next one may start one.
        out += ':';
        i = colon + 1;
    }
    out += text.substr(std::min(i, text.size()));
    return out;
}

std::vector<const Emoji*> search(std::string_view typed, std::size_t max, const std::vector<Emoji>& table) {
    const std::string q = lower(typed);
    // The better a match, the lower its rank: a name starting with it, a name with it, a tag or the description.
    std::vector<std::pair<int, const Emoji*>> found;
    for (const auto& e : table) {
        int rank = 4;
        for (const auto& alias : e.aliases) {
            rank = std::min(rank, alias.starts_with(q) ? 0 : alias.find(q) != std::string::npos ? 1 : 4);
        }
        for (const auto& tag : e.tags) {
            rank = std::min(rank, tag.starts_with(q) ? 2 : 4);
        }
        if (rank == 4 && lower(e.description).find(q) != std::string::npos) {
            rank = 3;
        }
        if (rank < 4) {
            found.emplace_back(rank, &e);
        }
    }
    // (Stable: in the list's order within a rank, the common ones first.)
    std::ranges::stable_sort(found, {}, &std::pair<int, const Emoji*>::first);
    std::vector<const Emoji*> out;
    for (std::size_t k = 0; k < found.size() && k < max; ++k) {
        out.push_back(found[k].second);
    }
    return out;
}

} // namespace zchat::emojis
