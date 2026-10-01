// Paint: a shared canvas of 16 x 16 squares that everybody in the chat colors, with /game paint c7 red (or a
// rectangle, /game paint c7-f9 blue). Without a color, the square gets the one of our brush: /game paint color red
// picks it, and it starts as the canvas color closest to our name color. There is no winner: the chat draws something together. Painting takes a second
// per square before the next change, so that nobody can fill the whole canvas at once.
//
// There is no referee either: every zchat keeps its own copy of the canvas, and they all end up the same. Every
// change of a square is stamped with a clock (one more than the highest one heard) and a tag made from the painter's
// id, and a square keeps the change with the highest clock, then tag: changes lost or heard out of order do not
// matter, as long as every one of them gets everywhere once. Every peer sends, as Game packets:
//   paint set <clock> <squares> <color>   a change of a square (c7) or a rectangle (c7-f9), tagged with the sender
//   paint fix <square>=<change> ...       taking back a change: the squares get back what they had before
//   paint cells <square>=<change> ...     the changes it has, for the others to catch up (not a change itself)
//   paint sum <clock> <hash>              every so often, while the canvas has anything: what its copy looks like
// where <clock> is a hex number, a <change> is <clock>.<tag>.<color> in hex, and <color> the index of one of the
// canvas colors. Whoever hears a sum that is not theirs sends its changes and its own sum, so newcomers, and whoever
// missed some changes, catch up.

#include "game.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <vector>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;

    constexpr std::string_view game_name = "paint";
    constexpr int size = 16;
    // How long painting a square makes us wait before the next change.
    constexpr auto square_cooldown = 1s;
    constexpr auto sum_interval = 15s;
    // Answering sums not like ours at most so often, so that a few peers catching up do not flood the chat.
    constexpr auto answer_interval = 3s;
    constexpr std::size_t max_undo = 20;
    // Room for the changes in a cells or fix packet, well within a packet's text.
    constexpr std::size_t max_entries_bytes = 900;

    struct CanvasColor {
        std::string_view name;
        Color color;
        char symbol; // without colors
    };
    // Index 0 is an empty square.
    constexpr auto colors = std::to_array<CanvasColor>({
        {"empty", {}, '.'},
        {"white", {240, 240, 240}, 'w'},
        {"gray", {128, 128, 128}, 'a'},
        {"black", {0, 0, 0}, 'k'},
        {"red", {230, 50, 50}, 'r'},
        {"orange", {255, 140, 0}, 'o'},
        {"yellow", {240, 220, 40}, 'y'},
        {"lime", {170, 240, 30}, 'l'},
        {"green", {60, 180, 75}, 'g'},
        {"cyan", {0, 200, 210}, 'c'},
        {"sky", {135, 195, 255}, 's'},
        {"blue", {50, 110, 240}, 'b'},
        {"violet", {150, 100, 255}, 'v'},
        {"magenta", {210, 40, 200}, 'm'},
        {"pink", {255, 130, 185}, 'p'},
        {"brown", {140, 85, 40}, 'n'},
    });
    // An empty square is shown as a checkerboard of these.
    constexpr Color empty_dark {38, 38, 38};
    constexpr Color empty_light {50, 50, 50};

    std::optional<std::uint64_t> parse_number(std::string_view s, int base = 10) {
        std::uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size()) {
            return std::nullopt;
        }
        return value;
    }

    // Takes the next word of s, up to a space, and removes it with the space.
    std::string_view next_field(std::string_view& s) {
        const auto space = s.find(' ');
        const std::string_view field = s.substr(0, space);
        s.remove_prefix(space == std::string_view::npos ? s.size() : space + 1);
        return field;
    }

    std::string plural(std::uint64_t n, std::string_view word) {
        return std::format("{} {}{}", n, word, n == 1 ? "" : "s");
    }

    // The tag of the changes of a peer: what breaks ties between changes with the same clock.
    std::uint16_t tag_of(std::uint64_t id) {
        return static_cast<std::uint16_t>(id ^ (id >> 16) ^ (id >> 32) ^ (id >> 48));
    }

    // A square as its index, from its name: a column from a to p and a row from 1 to 16, like c7.
    std::optional<int> parse_square(std::string_view s) {
        if (s.size() < 2 || s[0] < 'a' || s[0] >= 'a' + size) {
            return std::nullopt;
        }
        const auto row = parse_number(s.substr(1));
        if (!row || *row < 1 || *row > size) {
            return std::nullopt;
        }
        return static_cast<int>(*row - 1) * size + (s[0] - 'a');
    }

    std::string square_name(int index) {
        return std::format("{}{}", static_cast<char>('a' + index % size), index / size + 1);
    }

    struct Rect {
        int left = 0;
        int top = 0;
        int right = 0; // included
        int bottom = 0;

        int area() const {
            return (right - left + 1) * (bottom - top + 1);
        }
    };

    // A square (c7) or the rectangle between two corners (c7-f9, or f9-c7).
    std::optional<Rect> parse_rect(std::string_view s) {
        const auto dash = s.find('-');
        const auto a = parse_square(s.substr(0, dash));
        const auto b = dash == std::string_view::npos ? a : parse_square(s.substr(dash + 1));
        if (!a || !b) {
            return std::nullopt;
        }
        return Rect {std::min(*a % size, *b % size), std::min(*a / size, *b / size), std::max(*a % size, *b % size),
                     std::max(*a / size, *b / size)};
    }

    std::string rect_name(const Rect& r) {
        const std::string first = square_name(r.top * size + r.left);
        return r.area() == 1 ? first : std::format("{}-{}", first, square_name(r.bottom * size + r.right));
    }

    // The canvas color closest to a color (never empty).
    std::uint8_t closest_canvas_color(Color color) {
        const auto distance = [&](const CanvasColor& c) {
            const auto d = [](int x, int y) {
                return (x - y) * (x - y);
            };
            return d(c.color.r, color.r) + d(c.color.g, color.g) + d(c.color.b, color.b);
        };
        const auto it = std::ranges::min_element(colors.begin() + 1, colors.end(), {}, distance);
        return static_cast<std::uint8_t>(it - colors.begin());
    }

    // One of the canvas colors: by its name, or the closest one to any color zchat reads (a name color, #ff8800,
    // rgb(255, 136, 0)); none, erase and empty clear the squares.
    std::optional<std::uint8_t> parse_canvas_color(std::string_view s) {
        if (s == "none" || s == "erase" || s == "empty" || s == "clear") {
            return 0;
        }
        if (s == "grey") {
            s = "gray";
        }
        for (std::size_t i = 1; i < colors.size(); ++i) {
            if (colors[i].name == s) {
                return static_cast<std::uint8_t>(i);
            }
        }
        const auto color = parse_color(s);
        if (!color) {
            return std::nullopt;
        }
        return closest_canvas_color(*color);
    }

    // What a change did, after who did it: "painted c7-f9 red", or "emptied c7".
    std::string change_name(const Rect& r, std::uint8_t color) {
        return color == 0 ? std::format("emptied {}", rect_name(r))
                          : std::format("painted {} {}", rect_name(r), colors[color].name);
    }

    class Paint final : public Game {
    public:
        Paint(Chat& chat, Screen& terminal, Games& games) :
            chat_(chat),
            terminal_(terminal) {
            (void)games;
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "a shared canvas everyone colors square by square, to draw something together";
        }

        // Nobody wins a drawing.
        bool rated() const override {
            return false;
        }

        void start() override {
            if (!open_) {
                open_ = true;
                chat_.notice("🎨 The shared canvas: /game paint c7 red colors a square, /game paint c7-f9 blue a "
                             "rectangle, /game paint c7 none empties it, /game paint undo takes back your last change.");
                chat_.notice(std::format("   Without a color, /game paint c7 uses your brush, now {}: /game paint "
                                         "color red changes it.",
                                         colors[brush()].name));
                std::string names;
                for (std::size_t i = 1; i < colors.size(); ++i) {
                    names += std::format("{}{}", names.empty() ? "" : ", ", colors[i].name);
                }
                chat_.notice(std::format("   Colors: {} (or any color, like #ff8800: the closest is used). Each "
                                         "square painted takes a second before the next change.",
                                         names));
                // Catch up with the others right away, rather than at their next sum.
                send_sum();
            }
            draw("🎨 The shared canvas");
        }

        void receive(std::uint64_t sender, std::string_view name, std::string_view text) override {
            const std::string_view event = next_field(text);
            if (event == "set") {
                const auto stamp = parse_number(next_field(text), 16);
                const auto rect = parse_rect(next_field(text));
                const auto color = parse_number(next_field(text), 16);
                if (!stamp || *stamp == 0 || !rect || !color || *color >= colors.size()) {
                    return;
                }
                const Cell change {*stamp, tag_of(sender), static_cast<std::uint8_t>(*color)};
                bool changed = false;
                for (int row = rect->top; row <= rect->bottom; ++row) {
                    for (int col = rect->left; col <= rect->right; ++col) {
                        changed |= merge(row * size + col, change);
                    }
                }
                painted_by(sender, name, changed,
                           change_name(*rect, static_cast<std::uint8_t>(*color)));
            } else if (event == "fix") {
                painted_by(sender, name, merge_entries(text), "took back a change");
            } else if (event == "cells") {
                if (merge_entries(text) && open_) {
                    draw("🎨 The shared canvas, caught up with the others");
                }
            } else if (event == "sum") {
                const auto stamp = parse_number(next_field(text), 16);
                const auto hash = parse_number(next_field(text), 16);
                if (stamp && hash) {
                    seen(*stamp);
                    const auto now = clock::now();
                    if (*hash != hash_of_board() && now >= last_answer_ + answer_interval) {
                        last_answer_ = now;
                        send_cells();
                        send_sum();
                    }
                }
            }
        }

        void message(std::uint64_t, std::string_view, std::string_view) override {
        }

        void tick() override {
            const auto now = clock::now();
            if (now >= next_sum_) {
                next_sum_ = now + sum_interval;
                if (clock_ > 0) {
                    send_sum();
                }
            }
        }

        bool command(std::string_view args) override {
            if (args == "undo") {
                undo();
                return true;
            }
            std::string_view rest = args;
            const std::string_view first = next_field(rest);
            while (!rest.empty() && rest.front() == ' ') {
                rest.remove_prefix(1);
            }
            if (first == "color" || first == "colour") {
                pick_brush(rest);
                return true;
            }
            const auto rect = parse_rect(first);
            if (!rect) {
                return false;
            }
            const auto color = rest.empty() ? std::optional(brush()) : parse_canvas_color(rest);
            if (!color) {
                chat_.notice(std::format("Unknown color {}: try red, #ff8800 or none", rest));
                return true;
            }
            paint(*rect, *color);
            return true;
        }

        std::vector<Help> help() const override {
            return {
                {"", "show the canvas, and its colors"},
                {"c7 [COLOR]", "color a square (with your brush, without a color; none empties it)"},
                {"c7-f9 [COLOR]", "color a rectangle"},
                {"color COLOR", "pick your brush: red, #ff8800 (the closest canvas color)..."},
                {"undo", "take back your last change"},
            };
        }

    private:
        struct Cell {
            std::uint64_t clock = 0; // 0: never painted
            std::uint16_t tag = 0;
            std::uint8_t color = 0;

            bool operator==(const Cell&) const = default;
        };

        // A square before one of our changes, to take it back.
        struct Previous {
            int index = 0;
            Cell before;
            Cell ours;
        };

        // The color painted when none is given: the one picked, or the closest to our name color, which can change.
        std::uint8_t brush() const {
            return brush_.value_or(closest_canvas_color(chat_.color()));
        }

        // /game paint color COLOR; without one, tells which it is.
        void pick_brush(std::string_view name) {
            if (name.empty()) {
                const std::uint8_t color = brush();
                chat_.notice(color == 0 ? std::string("🎨 Your brush empties squares: /game paint color red changes it.")
                                        : std::format("🎨 Your brush is {}{}: /game paint color red changes it.",
                                                      colors[color].name,
                                                      brush_ ? "" : ", the closest to your name color"));
                return;
            }
            const auto color = parse_canvas_color(name);
            if (!color) {
                chat_.notice(std::format("Unknown color {}: try red, #ff8800 or none", name));
                return;
            }
            brush_ = *color;
            chat_.notice(*color == 0 ? std::string("🎨 Your brush now empties squares: /game paint c7 erases c7.")
                                     : std::format("🎨 Your brush is now {}: /game paint c7 paints c7 {}.",
                                                   colors[*color].name, colors[*color].name));
        }

        static bool newer(const Cell& a, const Cell& b) {
            return a.clock != b.clock ? a.clock > b.clock : a.tag > b.tag;
        }

        void seen(std::uint64_t stamp) {
            clock_ = std::max(clock_, stamp);
        }

        // Keeps a change of a square if it is newer than what it has; returns whether the square looks different.
        bool merge(int index, const Cell& change) {
            seen(change.clock);
            Cell& cell = board_[static_cast<std::size_t>(index)];
            if (!newer(change, cell)) {
                return false;
            }
            const bool changed = cell.color != change.color;
            cell = change;
            return changed;
        }

        // Merges <square>=<clock>.<tag>.<color> entries; returns whether the canvas looks different.
        bool merge_entries(std::string_view text) {
            bool changed = false;
            while (!text.empty()) {
                std::string_view entry = next_field(text);
                const auto equal = entry.find('=');
                const auto index = parse_square(entry.substr(0, equal));
                if (!index || equal == std::string_view::npos) {
                    continue;
                }
                entry.remove_prefix(equal + 1);
                const auto dot = entry.find('.');
                const auto stamp = parse_number(entry.substr(0, dot), 16);
                entry.remove_prefix(dot == std::string_view::npos ? entry.size() : dot + 1);
                const auto dot2 = entry.find('.');
                const auto tag = parse_number(entry.substr(0, dot2), 16);
                const auto color =
                    dot2 == std::string_view::npos ? std::nullopt : parse_number(entry.substr(dot2 + 1), 16);
                if (!stamp || *stamp == 0 || !tag || *tag > 0xFFFF || !color || *color >= colors.size()) {
                    continue;
                }
                changed |= merge(*index, {*stamp, static_cast<std::uint16_t>(*tag), static_cast<std::uint8_t>(*color)});
            }
            return changed;
        }

        // Shows a change right away, to whoever looks at the canvas: what someone did, like "painted c7 red".
        void painted_by(std::uint64_t sender, std::string_view name, bool changed, std::string_view what) {
            if (!changed) {
                return;
            }
            const std::string colored = chat_.colored_name(sender, name);
            if (open_) {
                draw(std::format("🎨 {} {}", colored, what));
            } else if (!invited_) {
                invited_ = true;
                chat_.notice(std::format("🎨 {} is painting on the shared canvas: /game paint shows it.", colored));
            }
        }

        void paint(const Rect& rect, std::uint8_t color) {
            const auto now = clock::now();
            if (now < next_paint_) {
                const auto wait = std::chrono::ceil<std::chrono::seconds>(next_paint_ - now);
                chat_.notice(std::format("🎨 Wait {} more to paint again.", plural(wait.count(), "second")));
                return;
            }
            open_ = true;
            const Cell change {clock_ + 1, tag_of(chat_.id()), color};
            std::vector<Previous> previous;
            for (int row = rect.top; row <= rect.bottom; ++row) {
                for (int col = rect.left; col <= rect.right; ++col) {
                    const int index = row * size + col;
                    previous.push_back({index, board_[static_cast<std::size_t>(index)], change});
                    merge(index, change);
                }
            }
            send(std::format("set {:x} {} {:x}", change.clock, rect_name(rect), color));
            undo_.push_back(std::move(previous));
            if (undo_.size() > max_undo) {
                undo_.erase(undo_.begin());
            }
            next_paint_ = now + square_cooldown * rect.area();
            painted_by(chat_.id(), chat_.name(), true,
                       change_name(rect, color));
        }

        void undo() {
            if (undo_.empty()) {
                chat_.notice("🎨 You have no change to take back.");
                return;
            }
            const std::vector<Previous> previous = std::move(undo_.back());
            undo_.pop_back();
            // The squares painted over since are left as they are. What they had before comes back as a new change,
            // so that it wins over ours everywhere.
            const std::uint16_t tag = tag_of(chat_.id());
            const std::uint64_t stamp = clock_ + 1;
            std::vector<std::pair<int, Cell>> fixes;
            for (const Previous& p : previous) {
                if (board_[static_cast<std::size_t>(p.index)] == p.ours) {
                    fixes.emplace_back(p.index, Cell {stamp, tag, p.before.color});
                }
            }
            if (fixes.empty()) {
                chat_.notice("🎨 Those squares have all been painted over since: nothing to take back.");
                return;
            }
            for (const auto& [index, cell] : fixes) {
                merge(index, cell);
            }
            send_entries("fix", fixes);
            painted_by(chat_.id(), chat_.name(), true, "took back a change");
        }

        std::uint64_t hash_of_board() const {
            // FNV-1a over every square's change.
            std::uint64_t hash = 14695981039346656037ULL;
            const auto add = [&](std::uint64_t v, int bytes) {
                for (int i = 0; i < bytes; ++i) {
                    hash = (hash ^ ((v >> (8 * i)) & 0xFF)) * 1099511628211ULL;
                }
            };
            for (const Cell& c : board_) {
                add(c.clock, 8);
                add(c.tag, 2);
                add(c.color, 1);
            }
            return hash;
        }

        void send_sum() {
            send(std::format("sum {:x} {:x}", clock_, hash_of_board()));
        }

        // Every square ever painted, for the others to catch up.
        void send_cells() {
            std::vector<std::pair<int, Cell>> cells;
            for (int i = 0; i < size * size; ++i) {
                if (board_[static_cast<std::size_t>(i)].clock > 0) {
                    cells.emplace_back(i, board_[static_cast<std::size_t>(i)]);
                }
            }
            send_entries("cells", cells);
        }

        // As many packets as needed.
        void send_entries(std::string_view event, const std::vector<std::pair<int, Cell>>& cells) {
            std::string entries;
            for (const auto& [index, cell] : cells) {
                entries += std::format("{}{}={:x}.{:x}.{:x}", entries.empty() ? "" : " ", square_name(index),
                                       cell.clock, cell.tag, cell.color);
                if (entries.size() >= max_entries_bytes) {
                    send(std::format("{} {}", event, entries));
                    entries.clear();
                }
            }
            if (!entries.empty()) {
                send(std::format("{} {}", event, entries));
            }
        }

        void draw(const std::string& header) const {
            const bool painted = std::ranges::any_of(board_, [](const Cell& c) {
                return c.color != 0;
            });
            chat_.notice(painted ? header + ":" : header + ", still empty:");

            // Every square is two characters wide, which makes it about square in most fonts.
            const bool vt = terminal_.colors();
            std::string out = "        ";
            for (int col = 0; col < size; ++col) {
                out += std::format("{} ", static_cast<char>('a' + col));
            }
            for (int row = 0; row < size; ++row) {
                out += std::format("\n     {:>2} ", row + 1);
                for (int col = 0; col < size; ++col) {
                    const std::uint8_t color = board_[static_cast<std::size_t>(row * size + col)].color;
                    if (!vt) {
                        out += std::format("{} ", colors[color].symbol);
                        continue;
                    }
                    const Color c = color != 0 ? colors[color].color : (row + col) % 2 ? empty_light : empty_dark;
                    out += std::format("\x1b[48;2;{};{};{}m  ", c.r, c.g, c.b);
                }
                out += vt ? std::format("\x1b[0m {}", row + 1) : std::format("{}", row + 1);
            }
            if (!vt) {
                std::string legend;
                for (const CanvasColor& c : colors) {
                    legend += std::format("{}{} {}", legend.empty() ? "" : ", ", c.symbol, c.name);
                }
                out += "\n        " + legend;
            }
            terminal_.print(out);
        }

        void send(std::string_view event) {
            chat_.send_game(std::format("{} {}", game_name, event));
        }

        Chat& chat_;
        Screen& terminal_;

        std::array<Cell, size * size> board_ {};
        // The highest clock heard: our next change gets one more.
        std::uint64_t clock_ = 0;
        // Whether we have looked at the canvas or painted on it: then it is shown again, right away, on every change.
        bool open_ = false;
        // Whether we were told someone is painting, while not looking at the canvas.
        bool invited_ = false;
        clock::time_point next_paint_;
        clock::time_point next_sum_ = clock::now() + sum_interval;
        clock::time_point last_answer_;
        std::vector<std::vector<Previous>> undo_;
        // Picked with /game paint color; until then, see brush().
        std::optional<std::uint8_t> brush_;
    };

} // namespace

std::unique_ptr<Game> make_paint(Chat& chat, Screen& terminal, Games& games) {
    return std::make_unique<Paint>(chat, terminal, games);
}

} // namespace zchat::game
