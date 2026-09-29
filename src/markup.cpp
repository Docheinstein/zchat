#include "markup.hpp"

#include "color.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <vector>

namespace zchat::markup {

namespace {

    constexpr std::array all_tags = std::to_array<Tag>({
        {
            "color",
            "colors the text",
            "Colors the text up to </color>, or to the end of the message. The color is a name from /color "
            "(red, magenta, blue, ...), a hex code (#ff8800 or #f80) or RGB values (255,136,0). Tags can be "
            "nested: </color> goes back to the color before.",
            "this is a <color=red>test</color>, and <color=#ff8800>orange <color=0,200,255>blue</color> "
            "orange again</color>",
        },
    });

    // Plain text color, used after the last color tag is closed.
    constexpr std::string_view default_foreground = "\x1b[39m";

    std::string lowercase(std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        return out;
    }

    std::string_view trim(std::string_view s) {
        while (!s.empty() && s.front() == ' ') {
            s.remove_prefix(1);
        }
        while (!s.empty() && s.back() == ' ') {
            s.remove_suffix(1);
        }
        return s;
    }

    // A tag between '<' and '>', e.g. "color=red" or "/color".
    struct ParsedTag {
        bool closing = false;
        std::optional<Color> color; // for an opening color tag
    };

    std::optional<ParsedTag> parse_tag(std::string_view inside) {
        ParsedTag tag;
        if (inside.starts_with('/')) {
            tag.closing = true;
            inside.remove_prefix(1);
            return lowercase(trim(inside)) == "color" ? std::optional(tag) : std::nullopt;
        }
        const auto eq = inside.find('=');
        if (eq == std::string_view::npos || lowercase(trim(inside.substr(0, eq))) != "color") {
            return std::nullopt;
        }
        std::string_view value = trim(inside.substr(eq + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
            value = trim(value.substr(1, value.size() - 2));
        }
        tag.color = parse_color(value);
        if (!tag.color) {
            return std::nullopt;
        }
        return tag;
    }

} // namespace

std::span<const Tag> tags() {
    return all_tags;
}

const Tag* find_tag(std::string_view name) {
    const std::string wanted = lowercase(trim(name));
    for (const auto& tag : all_tags) {
        if (tag.name == wanted) {
            return &tag;
        }
    }
    return nullptr;
}

std::string render(std::string_view text, bool colors) {
    std::string out;
    std::vector<Color> stack;
    const auto set_color = [&] {
        if (colors) {
            out += stack.empty() ? std::string(default_foreground) : "\x1b[" + ansi_foreground(stack.back()) + "m";
        }
    };
    while (!text.empty()) {
        const auto lt = text.find('<');
        out += text.substr(0, lt);
        if (lt == std::string_view::npos) {
            break;
        }
        text.remove_prefix(lt);
        const auto gt = text.find('>');
        // Another '<' before the '>' means this one is just text, e.g. "a <3 <color=red>b".
        const auto next_lt = text.find('<', 1);
        const auto tag = gt != std::string_view::npos && (next_lt == std::string_view::npos || next_lt > gt)
                             ? parse_tag(text.substr(1, gt - 1))
                             : std::nullopt;
        if (!tag) {
            out += '<';
            text.remove_prefix(1);
            continue;
        }
        text.remove_prefix(gt + 1);
        if (tag->closing) {
            // A </color> with nothing open is ignored, like in HTML.
            if (!stack.empty()) {
                stack.pop_back();
                set_color();
            }
        } else {
            stack.push_back(*tag->color);
            set_color();
        }
    }
    // Colors end with the message.
    if (colors && !stack.empty()) {
        out += default_foreground;
    }
    return out;
}

} // namespace zchat::markup
