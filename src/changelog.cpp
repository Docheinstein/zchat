#include "changelog.hpp"

namespace zchat::changelog {

namespace {

    std::string_view trim(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
            s.remove_suffix(1);
        }
        return s;
    }

} // namespace

std::vector<Day> parse(std::string_view markdown) {
    std::vector<Day> days;
    bool in_item = false;
    while (!markdown.empty()) {
        const auto end = markdown.find('\n');
        std::string_view line = markdown.substr(0, end);
        markdown.remove_prefix(end == std::string_view::npos ? markdown.size() : end + 1);
        if (line.starts_with("## ")) {
            days.push_back({std::string(trim(line.substr(3))), {}});
            in_item = false;
        } else if (days.empty()) {
            continue;
        } else if (line.starts_with("* ") || line.starts_with("- ")) {
            days.back().items.emplace_back(trim(line.substr(2)));
            in_item = true;
        } else if (in_item && (line.starts_with(' ') || line.starts_with('\t')) && !trim(line).empty()) {
            // The item goes on.
            days.back().items.back() += ' ';
            days.back().items.back() += trim(line);
        } else {
            in_item = false;
        }
    }
    return days;
}

std::string plain(std::string_view item) {
    std::string out;
    for (std::size_t i = 0; i < item.size(); ++i) {
        if (item.substr(i).starts_with("**")) {
            ++i;
        } else if (item[i] != '`') {
            out += item[i];
        }
    }
    return out;
}

std::string headline(std::string_view item) {
    if (item.starts_with("**")) {
        if (const auto end = item.find("**", 2); end != std::string_view::npos && end > 2) {
            return plain(item.substr(2, end - 2));
        }
    }
    std::string text = plain(item);
    if (const auto colon = text.find(": "); colon != std::string::npos && colon <= 60) {
        text.resize(colon);
    }
    if (text.size() > 60) {
        // Shortened at a space, so that no word (nor UTF-8 character) is cut.
        const auto space = text.rfind(' ', 60);
        text.resize(space == std::string::npos ? 60 : space);
        text += "…";
    }
    while (!text.empty() && (text.back() == '.' || text.back() == ',')) {
        text.pop_back();
    }
    return text;
}

std::vector<std::string> news_since(const std::vector<Day>& days, std::string_view seen, std::size_t max) {
    std::vector<std::string> news;
    for (const auto& day : days) {
        // ISO dates compare as strings.
        if (seen.empty() ? &day != &days.front() : day.date <= seen) {
            break;
        }
        for (const auto& item : day.items) {
            if (news.size() == max) {
                return news;
            }
            news.push_back(headline(item));
        }
    }
    return news;
}

} // namespace zchat::changelog
