#include "markup.hpp"

#include "color.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
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
        {
            "bold",
            "makes the text bold",
            "Makes the text bold up to </bold>, or to the end of the message. <b> is the short form. <bold=2> "
            "and <bold=3> are even bolder: terminals have a single bold weight, so they also make the text "
            "brighter. It can be mixed with the other tags.",
            "this is <bold>important</bold>, <b=2>more important</b>, <b=3><color=red>urgent!</color></b>",
            "b",
        },
        {
            "italic",
            "makes the text italic",
            "Makes the text italic up to </italic>, or to the end of the message. <i> is the short form. It can "
            "be mixed with the other tags. Some terminals, like the old Windows console, cannot show italic text.",
            "this is <italic>so</italic> nice, <i><b>really</b></i>",
            "i",
        },
        {
            "underscore",
            "underlines the text",
            "Underlines the text up to </underscore>, or to the end of the message. <u> is the short form. It "
            "can be mixed with the other tags.",
            "read <underscore>this</underscore> first, then <u><color=sky>that</color></u>",
            "u",
        },
        {
            "strikethrough",
            "strikes the text through",
            "Strikes the text through up to </strikethrough>, or to the end of the message. <s> is the short "
            "form. It can be mixed with the other tags. Some terminals, like the old Windows console, cannot show "
            "it.",
            "the meeting is on <s>Monday</s> Tuesday",
            "s",
        },
    });

    // In the same order as all_tags.
    enum class Kind { Color, Bold, Italic, Underscore, Strikethrough };

    // Plain text color, used after the last color tag is closed.
    constexpr std::string_view default_foreground = "\x1b[39m";

    // The tags that switch a style on and off, and their ANSI sequences.
    struct Style {
        std::string_view on;
        std::string_view off;
    };
    constexpr Kind first_style = Kind::Bold;
    constexpr std::array styles = std::to_array<Style>({
        {"\x1b[1m", "\x1b[22m"}, // bold
        {"\x1b[3m", "\x1b[23m"}, // italic
        {"\x1b[4m", "\x1b[24m"}, // underscore
        {"\x1b[9m", "\x1b[29m"}, // strikethrough
    });

    // <bold=2> and <bold=3>: terminals have a single bold weight, so higher levels also brighten the text.
    constexpr int max_bold_level = 3;
    // The usual text color of a terminal, brightened when no color tag is open.
    constexpr Color assumed_foreground {200, 200, 200};

    Color brighten(Color c, int bold_level) {
        const int percent = 35 * (bold_level - 1);
        const auto mix = [&](std::uint8_t v) {
            return static_cast<std::uint8_t>(v + (255 - v) * percent / 100);
        };
        return {mix(c.r), mix(c.g), mix(c.b)};
    }

    std::size_t style_index(Kind kind) {
        return static_cast<std::size_t>(kind) - static_cast<std::size_t>(first_style);
    }

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

    std::optional<Kind> find_kind(std::string_view name) {
        // The Kind values are in the order of all_tags.
        if (const Tag* tag = find_tag(name)) {
            return static_cast<Kind>(tag - all_tags.data());
        }
        return std::nullopt;
    }

    // A tag between '<' and '>', e.g. "color=red", "bold" or "/color".
    struct ParsedTag {
        Kind kind = Kind::Color;
        bool closing = false;
        std::optional<Color> color; // for an opening color tag
        int bold_level = 1;         // for an opening bold tag
    };

    std::optional<ParsedTag> parse_tag(std::string_view inside) {
        ParsedTag tag;
        const auto eq = inside.find('=');
        if (inside.starts_with('/') || eq == std::string_view::npos) {
            // A closing tag, or one without a value: only color needs one, bold may have one.
            tag.closing = inside.starts_with('/');
            const auto kind = find_kind(tag.closing ? inside.substr(1) : inside);
            if (!kind || (*kind == Kind::Color && !tag.closing)) {
                return std::nullopt;
            }
            tag.kind = *kind;
            return tag;
        }
        const auto kind = find_kind(inside.substr(0, eq));
        std::string_view value = trim(inside.substr(eq + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
            value = trim(value.substr(1, value.size() - 2));
        }
        if (kind == Kind::Bold) {
            tag.kind = Kind::Bold;
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), tag.bold_level);
            if (value.empty() || ec != std::errc {} || ptr != value.data() + value.size() || tag.bold_level < 1 ||
                tag.bold_level > max_bold_level) {
                return std::nullopt;
            }
            return tag;
        }
        if (kind != Kind::Color) {
            return std::nullopt;
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
        if (tag.name == wanted || (!tag.alias.empty() && tag.alias == wanted)) {
            return &tag;
        }
    }
    return nullptr;
}

std::string render(std::string_view text, bool colors) {
    std::string out;
    // Each kind of tag is opened and closed on its own, so they can overlap: <bold>a<italic>b</bold>c</italic>.
    std::vector<Color> color_stack;
    // How many tags of each style are open.
    std::array<int, styles.size()> open {};
    // The levels of the open bold tags; the highest one wins.
    std::vector<int> bold_levels;
    const auto bold_level = [&] {
        return bold_levels.empty() ? 1 : std::ranges::max(bold_levels);
    };
    const auto emit = [&](std::string_view sequence) {
        if (colors) {
            out += sequence;
        }
    };
    const auto set_color = [&] {
        const int level = bold_level();
        if (color_stack.empty() && level == 1) {
            emit(default_foreground);
            return;
        }
        const Color base = color_stack.empty() ? assumed_foreground : color_stack.back();
        emit("\x1b[" + ansi_foreground(brighten(base, level)) + "m");
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
        // A closing tag with nothing open is ignored, like in HTML.
        if (tag->kind == Kind::Color) {
            if (!tag->closing) {
                color_stack.push_back(*tag->color);
                set_color();
            } else if (!color_stack.empty()) {
                color_stack.pop_back();
                set_color();
            }
            continue;
        }
        if (tag->kind == Kind::Bold) {
            const int before = bold_level();
            if (!tag->closing) {
                bold_levels.push_back(tag->bold_level);
            } else if (!bold_levels.empty()) {
                bold_levels.pop_back();
            }
            if (bold_level() != before) {
                set_color();
            }
        }
        // Only the first style tag opened changes anything, only the last one closed ends it.
        const std::size_t s = style_index(tag->kind);
        if (!tag->closing) {
            if (open[s]++ == 0) {
                emit(styles[s].on);
            }
        } else if (open[s] > 0 && --open[s] == 0) {
            emit(styles[s].off);
        }
    }
    // Tags end with the message.
    if (!color_stack.empty() || bold_level() > 1) {
        emit(default_foreground);
    }
    for (std::size_t s = 0; s < styles.size(); ++s) {
        if (open[s] > 0) {
            emit(styles[s].off);
        }
    }
    return out;
}

} // namespace zchat::markup
