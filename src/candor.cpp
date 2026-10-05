// Radical Candor: a classic idle game, ZURU style, each on their own. Give feedback by hand for candor, then hire
// Developers who make it every second, HR Partners who hire Developers, Team Leads who hire HR, and so on up to the
// Board; upgrades, a feedback moment now and then, and an offsite to start over with more culture. The goal: so much
// candor that JavaScript runs out of numbers, Infinity, and then start over, stronger.
//
// The game itself is the window's (see the page): it keeps playing while the window is closed, and saves itself here
// with /game candor save, kept in "candor" in the config folder, so it goes on the next time zchat starts (and catches
// up on the time it was not running). Each player tells the others how they are doing, as a Game packet:
//   candor score <lifetime candor> <candor per second> [<Infinities>]   ("inf" once it is Infinity)
// and everybody keeps the leaderboard of what they heard. It needs the window: terminals only see the leaderboard.

#include "game.hpp"

#include "color.hpp"
#include "config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <vector>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;

    constexpr std::string_view game_name = "candor";
    // A save is base64 (of the page's JSON): nothing else is kept or handed back to the page.
    constexpr std::size_t max_save = 64 * 1024;
    // Players not heard from for so long leave the leaderboard.
    constexpr auto score_lifetime = 15min;

    // A number of candor: Infinity too, which is the goal (but not NaN).
    std::optional<double> parse_double(std::string_view s) {
        double value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size() || std::isnan(value) || value < 0) {
            return std::nullopt;
        }
        return value;
    }

    std::string_view next_field(std::string_view& s) {
        const auto space = s.find(' ');
        const std::string_view field = s.substr(0, space);
        s.remove_prefix(space == std::string_view::npos ? s.size() : space + 1);
        return field;
    }

    bool is_base64(std::string_view s) {
        return std::ranges::all_of(s, [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' || c == '=';
        });
    }

    std::string json(std::string_view s) {
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

    // 1234567 as "1.23M", like the window shows it.
    std::string short_number(double n) {
        static constexpr std::array<std::string_view, 12> suffixes {"",   "K",  "M",  "B",  "T",  "Qa",
                                                                    "Qi", "Sx", "Sp", "Oc", "No", "Dc"};
        if (std::isinf(n)) {
            return "∞";
        }
        if (n < 1000) {
            return std::format("{:.0f}", std::floor(n));
        }
        const int tier = static_cast<int>(std::floor(std::log10(n) / 3));
        if (tier >= static_cast<int>(suffixes.size())) {
            return std::format("{:.2e}", n);
        }
        return std::format("{:.2f}{}", n / std::pow(1000.0, tier), suffixes[static_cast<std::size_t>(tier)]);
    }

    class Candor final : public Game {
    public:
        Candor(Chat& chat, Screen& terminal) :
            chat_(chat),
            terminal_(terminal) {
            load();
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "Radical Candor, an idle game: give feedback, hire Developers, HR, Team Leads... for ever more candor";
        }

        // Played alone: no rounds, no winners, no ratings.
        bool rated() const override {
            return false;
        }

        bool keeps_case() const override {
            return true;
        }

        void start() override {
            if (!terminal_.show_game(game_name, state(true))) {
                chat_.notice("Radical Candor is played in zchat's window. /game candor top shows who has the most.");
            }
        }

        void receive(std::uint64_t sender, std::string_view name, std::string_view text) override {
            if (next_field(text) != "score") {
                return;
            }
            const auto total = parse_double(next_field(text));
            const auto rate = parse_double(next_field(text));
            const auto infinities = parse_double(text.empty() ? "0" : next_field(text));
            if (!total || !rate || !infinities || std::isinf(*infinities)) {
                return;
            }
            // By name: the sender id changes with the color.
            scores_[std::string(name)] = {sender, *total, *rate, static_cast<long long>(*infinities), clock::now()};
            publish();
        }

        void message(std::uint64_t, std::string_view, std::string_view) override {
        }

        void tick() override {
            // The saved game, for the window to go on with (and catch up), as soon as it can take it.
            if (!handed_ && !save_.empty()) {
                handed_ = terminal_.show_game(game_name, state(false));
            }
        }

        bool command(std::string_view args) override {
            const std::string_view what = next_field(args);
            if (what == "save") {
                if (args.size() <= max_save && is_base64(args)) {
                    save_ = std::string(args);
                    handed_ = true;
                    store();
                }
                return true;
            }
            if (what == "share") {
                // From the window: how we are doing, for everybody's leaderboard.
                const auto total = parse_double(next_field(args));
                const auto rate = parse_double(next_field(args));
                const auto infinities = parse_double(args.empty() ? "0" : next_field(args));
                if (total && rate && infinities && !std::isinf(*infinities)) {
                    const auto n = static_cast<long long>(*infinities);
                    scores_[chat_.name()] = {chat_.id(), *total, *rate, n, clock::now()};
                    chat_.send_game(std::format("{} score {} {} {}", game_name, number(*total), number(*rate), n));
                    publish();
                }
                return true;
            }
            if (what == "top") {
                top();
                return true;
            }
            return false;
        }

        std::vector<Help> help() const override {
            return {{"", "open the game (in the window)"}, {"top", "who has the most candor"}};
        }

    private:
        struct Score {
            std::uint64_t id = 0;
            double total = 0;
            double rate = 0;
            long long infinities = 0;
            clock::time_point heard;
        };

        // For a packet: "inf" once it is Infinity.
        static std::string number(double n) {
            return std::isinf(n) ? "inf" : std::format("{:.6g}", n);
        }

        // For the page: JavaScript reads 1e999 as Infinity (JSON has no Infinity).
        static std::string js_number(double n) {
            return std::isinf(n) ? "1e999" : std::format("{:.6g}", n);
        }

        std::filesystem::path file() const {
            const auto dir = config::dir();
            return dir.empty() ? dir : dir / "candor";
        }

        void load() {
            std::ifstream in(file());
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                text.pop_back();
            }
            if (text.size() <= max_save && is_base64(text)) {
                save_ = std::move(text);
            }
        }

        void store() const {
            if (const auto f = file(); !f.empty()) {
                std::ofstream(f, std::ios::trunc) << save_ << '\n';
            }
        }

        std::vector<std::pair<std::string, Score>> ranking() {
            const auto now = clock::now();
            std::erase_if(scores_, [&](const auto& entry) {
                return now - entry.second.heard > score_lifetime && entry.first != chat_.name();
            });
            std::vector<std::pair<std::string, Score>> out(scores_.begin(), scores_.end());
            // The most Infinities first, then the most candor.
            std::ranges::sort(out, [](const auto& a, const auto& b) {
                return a.second.infinities != b.second.infinities ? a.second.infinities > b.second.infinities
                                                                  : a.second.total > b.second.total;
            });
            return out;
        }

        // For the window: {"open": bool, "save": base64 or null, "leaderboard": [...]}.
        std::string state(bool open) {
            std::string board = "[";
            for (const auto& [name, s] : ranking()) {
                const Color c = color_of_id(s.id);
                board += std::format("{}{{\"name\":{},\"color\":\"#{:02x}{:02x}{:02x}\",\"total\":{},\"rate\":{},"
                                     "\"infinities\":{},\"you\":{}}}",
                                     board.size() > 1 ? "," : "", json(name), c.r, c.g, c.b, js_number(s.total),
                                     js_number(s.rate), s.infinities, name == chat_.name());
            }
            board += "]";
            return std::format("{{\"open\":{},\"save\":{},\"leaderboard\":{}}}", open,
                               save_.empty() ? "null" : json(save_), board);
        }

        void publish() {
            // Only the leaderboard: the window has its game.
            std::string s = state(false);
            terminal_.show_game(game_name, s);
        }

        void top() {
            const auto board = ranking();
            if (board.empty()) {
                chat_.notice("Nobody has shared any candor yet: /game candor to play.");
                return;
            }
            std::string list;
            int place = 0;
            for (const auto& [name, s] : board) {
                if (++place > 10) {
                    break;
                }
                const std::string infinities = s.infinities ? std::format("∞×{} ", s.infinities) : "";
                list += std::format("{}{}. {} {}{} ({}/s)", list.empty() ? "" : ", ", place, chat_.colored_name(s.id, name),
                                    infinities, short_number(s.total), short_number(s.rate));
            }
            chat_.notice(std::format("🗣️ Most Radical Candor: {}", list));
        }

        Chat& chat_;
        Screen& terminal_;
        std::string save_;
        // Whether the window has the saved game.
        bool handed_ = false;
        std::map<std::string, Score> scores_;
    };

} // namespace

std::unique_ptr<Game> make_candor(Chat& chat, Screen& terminal, Games&) {
    return std::make_unique<Candor>(chat, terminal);
}

} // namespace zchat::game
