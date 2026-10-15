// Wordle: the New York Times' puzzle of the day, played by the whole chat. Everyone guesses the same five-letter word
// on their own, with /game wordle WORD, whenever they like during the day, and sees which letters are in the word; the
// others only see the colors, as when sharing a Wordle. At midnight, when the Times has a new word, whoever found the
// word in the fewest guesses wins the day (together, between equals).
//
// As on the Times, there is one Wordle a day, and it is played once: our guesses are saved in the config file, as
// "wordle=<date> <word>,<word>...", so that quitting zchat and coming back goes on from them, and once the word is
// found or the guesses are over it cannot be played again until tomorrow.
//
// The word comes from the New York Times: https://www.nytimes.com/svc/wordle/v2/YYYY-MM-DD.json gives the puzzle of
// that day ({"id":829,"solution":"river","print_date":"2026-09-30",...}), fetched with curl, which Windows 10 and
// later, macOS and most Linux have. Everyone gets it on their own, the first time they play that day. Guesses must be
// words of the list in third_party/enable (or the Times' word itself).
//
// There is no referee: the day is the round. Every player sends, as Game packets:
//   wordle guess <date> <n> <marks>   guess n (from 1) of the Wordle of that date, and its marks (g: right place,
//                                     y: in the word, b: not in it): "ggggg" found the word
//   wordle lost <date>                no guesses left
//   wordle ask <date>                 we just started playing that day: who played it already?
//   wordle result <date> <n> <state>  answer to ask: n guesses so far, and whether the player is still guessing (g),
//                                     found the word (s) or lost (l)
// Everybody ends the day on their own clock, at midnight, and shows who won.
//
// Who played a day, and how, is kept as records (see Chat::record()), "DATE.WHO.N" for "guess DATE N MARKS" and
// "DATE.WHO.x" for "lost DATE" (WHO is the user id in hex, or "s" and the sender id), which the zchats sync: the end of
// a day is an update due at its midnight (see Chat::schedule()), so it happens even if nobody was online then, as soon
// as somebody is, from the records of whoever played. Each zchat ends each day once ("wordle_settled" in the config),
// and so pays its own user (see game::Ratings).

#include "game.hpp"

#include "config.hpp"
#include "process.hpp"
#include "wordle_words.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <ctime>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;

    constexpr std::string_view game_name = "wordle";
    constexpr std::size_t letters = 5;
    constexpr std::uint64_t max_guesses = 6;
    constexpr auto fetch_timeout = 15s;
    constexpr std::string_view saved_key = "wordle";
    // The days ended already (see settle()), and the updates that end them (see Chat::schedule()).
    constexpr std::string_view settled_key = "wordle_settled";
    constexpr std::size_t settled_kept = 60;
    constexpr std::string_view end_prefix = "wordle-end ";
    // How late the end of a day must be to say why it comes now.
    constexpr std::time_t late_after = 2 * 60;


    struct Tile {
        std::string_view emoji;
        Color color;
    };
    // By mark, the colors of the Times' own tiles.
    constexpr Tile right_tile {"🟩", {83, 141, 78}};
    constexpr Tile close_tile {"🟨", {181, 159, 59}};
    constexpr Tile wrong_tile {"⬛", {58, 58, 60}};

    const Tile& tile(char mark) {
        return mark == 'g' ? right_tile : mark == 'y' ? close_tile : wrong_tile;
    }

    std::optional<std::uint64_t> parse_number(std::string_view s, int base = 10) {
        std::uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size()) {
            return std::nullopt;
        }
        return value;
    }

    // Midnight at the end of a day (YYYY-MM-DD), on our clock; 0 when it is not a date.
    std::time_t end_of(std::string_view date) {
        const auto y = date.size() == 10 ? parse_number(date.substr(0, 4)) : std::nullopt;
        const auto m = date.size() == 10 ? parse_number(date.substr(5, 2)) : std::nullopt;
        const auto d = date.size() == 10 ? parse_number(date.substr(8, 2)) : std::nullopt;
        if (!y || !m || !d) {
            return 0;
        }
        std::tm tm {};
        tm.tm_year = static_cast<int>(*y) - 1900;
        tm.tm_mon = static_cast<int>(*m) - 1;
        tm.tm_mday = static_cast<int>(*d) + 1;
        tm.tm_isdst = -1;
        const std::time_t t = std::mktime(&tm);
        return t == static_cast<std::time_t>(-1) ? 0 : t;
    }

    // Takes the next word of s, up to a space, and removes it with the space.
    std::string_view next_field(std::string_view& s) {
        const auto space = s.find(' ');
        const std::string_view field = s.substr(0, space);
        s.remove_prefix(space == std::string_view::npos ? s.size() : space + 1);
        return field;
    }

    bool is_word(std::string_view s) {
        return s.size() == letters && std::ranges::all_of(s, [](char c) {
                   return c >= 'a' && c <= 'z';
               });
    }

    // The words that can be guessed, from third_party/enable/words5.txt, sorted: split on first use.
    const std::vector<std::string_view>& dictionary() {
        static const std::vector<std::string_view> words = [] {
            std::vector<std::string_view> out;
            std::string_view rest(reinterpret_cast<const char*>(wordle_words), wordle_words_size);
            while (!rest.empty()) {
                const auto end = rest.find_first_of("\r\n");
                if (const std::string_view word = rest.substr(0, end); !word.empty()) {
                    out.push_back(word);
                }
                rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
            }
            std::ranges::sort(out);
            return out;
        }();
        return words;
    }

    std::string guesses(std::uint64_t n) {
        return std::format("{} guess{}", n, n == 1 ? "" : "es");
    }

    // The marks of a guess, as Wordle gives them: a letter in the word but in the wrong place is only marked 'y' as
    // many times as the word has it, not counting the ones in the right place.
    std::string marks(std::string_view guess, std::string_view answer) {
        std::string out(letters, 'b');
        std::array<int, 26> left {};
        for (std::size_t i = 0; i < letters; ++i) {
            if (guess[i] == answer[i]) {
                out[i] = 'g';
            } else {
                ++left[static_cast<std::size_t>(answer[i] - 'a')];
            }
        }
        for (std::size_t i = 0; i < letters; ++i) {
            if (out[i] != 'g' && left[static_cast<std::size_t>(guess[i] - 'a')] > 0) {
                --left[static_cast<std::size_t>(guess[i] - 'a')];
                out[i] = 'y';
            }
        }
        return out;
    }
    std::chrono::year_month_day today() {
        const std::time_t t = std::time(nullptr);
        std::tm local {};
#ifdef _WIN32
        localtime_s(&local, &t);
#else
        localtime_r(&t, &local);
#endif
        return {std::chrono::year(local.tm_year + 1900), std::chrono::month(static_cast<unsigned>(local.tm_mon + 1)),
                std::chrono::day(static_cast<unsigned>(local.tm_mday))};
    }

    std::string iso_date(std::chrono::year_month_day d) {
        return std::format("{:04}-{:02}-{:02}", static_cast<int>(d.year()), static_cast<unsigned>(d.month()),
                           static_cast<unsigned>(d.day()));
    }

    // The value of a field of the Times' JSON, a string or a number, without its quotes; empty when it is not there.
    std::string_view json_field(std::string_view json, std::string_view key) {
        const std::string quoted = std::format("\"{}\"", key);
        auto at = json.find(quoted);
        if (at == std::string_view::npos) {
            return {};
        }
        at = json.find(':', at + quoted.size());
        if (at == std::string_view::npos) {
            return {};
        }
        json.remove_prefix(at + 1);
        while (!json.empty() && (json.front() == ' ' || json.front() == '"')) {
            json.remove_prefix(1);
        }
        return json.substr(0, json.find_first_of("\",} "));
    }

    std::string today_date() {
        return iso_date(today());
    }

    bool is_date(std::string_view s) {
        return s.size() == 10 && s[4] == '-' && s[7] == '-' && std::ranges::all_of(s, [](char c) {
                   return c == '-' || (c >= '0' && c <= '9');
               });
    }

    struct Puzzle {
        std::uint64_t id = 0;
        std::string date;
        std::string word;
    };

    class Wordle final : public Game {
    public:
        Wordle(Chat& chat, Screen& terminal, Games& games) :
            chat_(chat),
            terminal_(terminal),
            games_(games),
            day_(today_date()) {
            chat_.keep_game(game_name);
            // The days ended already.
            std::string_view rest;
            const auto value = config::get(settled_key);
            if (value) {
                rest = *value;
            }
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                if (const std::string_view date = rest.substr(0, comma); is_date(date)) {
                    settled_.emplace_back(date);
                }
                rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 1);
            }
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "the New York Times' Wordle of the day: guess the word in six tries, until midnight; the fewest "
                   "guesses win the day";
        }

        // /game wordle: gets today's word, or shows how the day is going.
        void start() override {
            if (puzzle_) {
                if (!board_.empty()) {
                    draw();
                }
                if (const Player* me = own(); me && me->status != Status::Guessing) {
                    played_already();
                } else {
                    chat_.game_notice(std::format(
                        "{} of Wordle {}: /game wordle WORD to guess.",
                        board_.empty() ? "6 guesses" : guesses(max_guesses - board_.size()) + " left", puzzle_->id));
                }
                standings();
                return;
            }
            fetch();
        }

        void receive(std::uint64_t sender, std::string_view name, std::string_view text) override {
            const std::string_view event = next_field(text);
            const std::string_view date = next_field(text);
            if (!is_date(date)) {
                return;
            }
            remember(sender, name, event, date, text);
            // Another day's (another time zone, or a round of an older zchat): not ours to show.
            if (date != day_) {
                return;
            }
            if (event == "guess") {
                const auto n = parse_number(next_field(text));
                const std::string_view mark = next_field(text);
                if (n && *n >= 1 && *n <= max_guesses && mark.size() == letters &&
                    mark.find_first_not_of("gyb") == std::string_view::npos) {
                    guessed(player(sender, name), *n, mark);
                }
            } else if (event == "lost") {
                lost(player(sender, name));
            } else if (event == "ask") {
                if (const Player* me = own()) {
                    send(std::format("result {} {} {}", day_, me->guesses, state(me->status)));
                }
            } else if (event == "result") {
                const auto n = parse_number(next_field(text));
                const std::string_view s = next_field(text);
                if (n && *n <= max_guesses && s.size() == 1 && std::string_view("gsl").find(s[0]) != std::string_view::npos) {
                    Player& p = player(sender, name);
                    if (*n >= p.guesses && p.status == Status::Guessing) {
                        p.guesses = *n;
                        p.status = s[0] == 's' ? Status::Solved : s[0] == 'l' ? Status::Lost : Status::Guessing;
                    }
                }
            }
        }

        void message(std::uint64_t, std::string_view, std::string_view) override {
        }

        void tick() override {
            // What we know of the days (ours, the others', heard now or synced) makes their end due at midnight.
            if (const auto version = chat_.records_version(); version != records_seen_) {
                records_seen_ = version;
                schedule_ends();
            }
            for (const auto& key : chat_.take_due(end_prefix)) {
                settle(key.substr(end_prefix.size()));
            }
            if (const std::string now = today_date(); now != day_) {
                new_day();
                day_ = now;
            }
            if (auto puzzle = fetched()) {
                if (puzzle->date == day_) {
                    begin(std::move(*puzzle));
                }
            }
        }

        bool command(std::string_view args) override {
            // Any one word is a guess, so that a wrong one is told why.
            if (args.find(' ') != std::string_view::npos) {
                return false;
            }
            if (!puzzle_) {
                // The first guess of the day: it is made once the word comes.
                pending_ = std::string(args);
                fetch();
                return true;
            }
            guess(args);
            return true;
        }

        std::vector<Help> help() const override {
            return {
                {"", "get today's word, or show how the day is going"},
                {"WORD", "guess the five-letter word: you see which letters are in it, the others only the colors"},
            };
        }

    private:
        enum class Status { Guessing, Solved, Lost };

        struct Player {
            std::uint64_t id = 0;
            std::string name;
            std::string colored_name;
            Status status = Status::Guessing;
            std::uint64_t guesses = 0;
        };

        static char state(Status s) {
            return s == Status::Solved ? 's' : s == Status::Lost ? 'l' : 'g';
        }

        static int rank(char mark) {
            return mark == 'g' ? 2 : mark == 'y' ? 1 : 0;
        }

        // Asks the Times for today's puzzle, in the background: tick() starts playing when it comes.
        void fetch() {
            std::scoped_lock lock(fetch_mutex_);
            if (fetching_) {
                chat_.game_notice("The Wordle is on its way from the New York Times…");
                return;
            }
            fetching_ = true;
            const std::string date = day_;
            chat_.game_notice(std::format("📰 Getting the Wordle of {} from the New York Times…", date));
            if (fetcher_.joinable()) {
                fetcher_.join();
            }
            fetcher_ = std::jthread([this, date](std::stop_token stop) {
                const auto result =
                    process::run({"curl", "-s", "-f", "-L", "-m", "10",
                                  std::format("https://www.nytimes.com/svc/wordle/v2/{}.json", date)},
                                 stop, fetch_timeout);
                std::string word(json_field(result.output, "solution"));
                std::ranges::transform(word, word.begin(), [](char c) {
                    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
                });
                const auto id = parse_number(json_field(result.output, "id"));
                std::scoped_lock lock(fetch_mutex_);
                if (stop.stop_requested()) {
                    return;
                }
                if (result.exit_code == 0 && is_word(word)) {
                    fetched_ = Puzzle {id.value_or(0), date, word};
                } else {
                    failed_ = true;
                }
            });
        }

        // The puzzle, once it came; tells when it could not come.
        std::optional<Puzzle> fetched() {
            std::scoped_lock lock(fetch_mutex_);
            if (failed_) {
                failed_ = false;
                fetching_ = false;
                pending_.reset();
                chat_.game_notice("Could not get the Wordle from the New York Times: is curl installed, and is there "
                                  "internet?");
            }
            if (!fetched_) {
                return std::nullopt;
            }
            fetching_ = false;
            return std::exchange(fetched_, std::nullopt);
        }

        // Today's word came: we play, from our saved guesses if we played earlier.
        void begin(Puzzle puzzle) {
            puzzle_ = std::move(puzzle);
            board_.clear();
            for (const std::string& word : saved()) {
                if (board_.size() == max_guesses || (!board_.empty() && board_.back().second == "ggggg")) {
                    break;
                }
                board_.push_back({word, marks(word, puzzle_->word)});
            }
            if (!board_.empty()) {
                Player& me = player(chat_.id(), chat_.name());
                me.guesses = board_.size();
                me.status = board_.back().second == "ggggg" ? Status::Solved
                            : board_.size() == max_guesses  ? Status::Lost
                                                            : Status::Guessing;
            }
            send(std::format("ask {}", day_));

            const Player* me = own();
            if (!me) {
                chat_.game_notice(
                    std::format("🟩 Wordle {} ({}): guess the five-letter word with /game wordle WORD, "
                                "until midnight. You see which letters are in it, the others only see your "
                                "colors; the fewest guesses win the day.",
                                puzzle_->id, puzzle_->date));
            } else if (me->status == Status::Guessing) {
                chat_.game_notice(std::format("🟩 Wordle {}: you started it earlier, {} left.", puzzle_->id,
                                              guesses(max_guesses - board_.size())));
                // The guess waiting, if any, draws the board anyway.
                if (!pending_) {
                    draw();
                }
            } else if (!pending_) {
                draw();
                played_already();
            }
            if (pending_) {
                guess(*std::exchange(pending_, std::nullopt));
            }
        }

        void played_already() const {
            chat_.game_notice("You played today's Wordle already: there is one a day, a new one at midnight!");
        }

        // Our guesses of today's Wordle, as saved; none if they are another day's.
        std::vector<std::string> saved() const {
            std::vector<std::string> words;
            const auto value = config::get(saved_key);
            if (!value || !value->starts_with(day_) || value->size() <= day_.size() || (*value)[day_.size()] != ' ') {
                return words;
            }
            std::string_view rest = std::string_view(*value).substr(day_.size() + 1);
            // Older versions marked the end.
            if (rest.ends_with(" done")) {
                rest.remove_suffix(5);
            }
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                if (const std::string_view word = rest.substr(0, comma); is_word(word)) {
                    words.emplace_back(word);
                }
                rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 1);
            }
            return words;
        }

        void save() const {
            std::string words;
            for (const auto& [word, mark] : board_) {
                words += std::format("{}{}", words.empty() ? "" : ",", word);
            }
            if (!config::set(saved_key, std::format("{} {}", day_, words))) {
                chat_.game_notice(std::format("Could not save your Wordle to {}", config::file().string()));
            }
        }

        // Us among the players, once we guessed.
        const Player* own() const {
            const auto it = std::ranges::find(players_, chat_.id(), &Player::id);
            return it == players_.end() ? nullptr : &*it;
        }

        // The player, added if we had not heard of them.
        Player& player(std::uint64_t id, std::string_view name) {
            auto it = std::ranges::find(players_, id, &Player::id);
            if (it == players_.end()) {
                players_.push_back({id, std::string(name), chat_.colored_name(id, name)});
                it = players_.end() - 1;
            }
            return *it;
        }

        void guess(std::string_view word) {
            if (!is_word(word)) {
                chat_.game_notice("A guess is a word of five letters, from a to z.");
                return;
            }
            if (const Player* me = own(); me && me->status != Status::Guessing) {
                played_already();
                return;
            }
            // The Times' word always counts, even if the list does not have it.
            if (word != puzzle_->word && !std::ranges::binary_search(dictionary(), word)) {
                chat_.game_notice(std::format("{} is not in the word list: it does not count as a guess.", big(word)));
                return;
            }
            const std::string mark = marks(word, puzzle_->word);
            board_.push_back({std::string(word), mark});
            save();
            send(std::format("guess {} {} {}", day_, board_.size(), mark));
            chat_.record(game_name, std::format("{}.{}.{}", day_, who(chat_.user(), chat_.id()), board_.size()),
                         std::format("guess {} {} {}", day_, board_.size(), mark));
            draw();
            Player& me = player(chat_.id(), chat_.name());
            guessed(me, board_.size(), mark);
            if (me.status == Status::Guessing && board_.size() == max_guesses) {
                chat_.game_notice(
                    std::format("No guesses left! The word was {}. A new Wordle at midnight.", big(puzzle_->word)));
                send(std::format("lost {}", day_));
                chat_.record(game_name, std::format("{}.{}.x", day_, who(chat_.user(), chat_.id())),
                             std::format("lost {}", day_));
                lost(me);
            } else if (me.status == Status::Solved) {
                chat_.game_notice("The winner of the day is told at midnight: /game wordle shows how the others do.");
            }
        }

        // Our guesses, as the Times shows them, and the letters of the alphabet by what we know of them.
        void draw() const {
            const bool vt = terminal_.colors();
            std::string out;
            for (const auto& [word, mark] : board_) {
                out += out.empty() ? "     " : "\n     ";
                for (std::size_t i = 0; i < letters; ++i) {
                    const char letter = static_cast<char>(word[i] - 'a' + 'A');
                    const Color c = tile(mark[i]).color;
                    out += vt ? std::format("\x1b[1;97;48;2;{};{};{}m {} \x1b[0m ", c.r, c.g, c.b, letter)
                              : std::format("{}{} ", tile(mark[i]).emoji, letter);
                }
            }
            // The best mark of each letter guessed.
            std::array<char, 26> known {};
            for (const auto& [word, mark] : board_) {
                for (std::size_t i = 0; i < letters; ++i) {
                    char& k = known[static_cast<std::size_t>(word[i] - 'a')];
                    if (k == 0 || rank(mark[i]) > rank(k)) {
                        k = mark[i];
                    }
                }
            }
            out += "\n";
            for (std::string_view row : {"qwertyuiop", "asdfghjkl", "zxcvbnm"}) {
                out += row.size() == 10 ? "\n     " : row.size() == 9 ? "\n      " : "\n        ";
                for (const char letter : row) {
                    const char k = known[static_cast<std::size_t>(letter - 'a')];
                    const char upper = static_cast<char>(letter - 'a' + 'A');
                    if (k == 0) {
                        out += std::format("{} ", upper);
                    } else if (!vt) {
                        out += k == 'b' ? std::string("  ") : std::format("{}{} ", tile(k).emoji, upper);
                    } else {
                        const Color c = tile(k).color;
                        out += std::format("\x1b[{}38;2;{};{};{}m{}\x1b[0m ", k == 'b' ? "9;" : "1;", c.r, c.g, c.b,
                                           upper);
                    }
                }
            }
            chat_.game_print(out);
        }

        std::string big(std::string_view word) const {
            std::string upper(word);
            std::ranges::transform(upper, upper.begin(), [](char c) {
                return static_cast<char>(c - 'a' + 'A');
            });
            return terminal_.colors() ? std::format("\x1b[1m{}\x1b[0m", upper) : upper;
        }

        std::string emoji(std::string_view mark) const {
            std::string out;
            for (const char m : mark) {
                out += tile(m).emoji;
            }
            return out;
        }

        void guessed(Player& p, std::uint64_t n, std::string_view mark) {
            if (p.status != Status::Guessing || n <= p.guesses) {
                return;
            }
            p.guesses = n;
            if (mark == "ggggg") {
                p.status = Status::Solved;
                chat_.game_notice(std::format("🎉 {} found today's Wordle in {} ({}/{})!", p.colored_name, guesses(n),
                                              n, max_guesses));
            } else if (p.id != chat_.id()) {
                chat_.game_notice(std::format("{} {} ({}/{}){}", emoji(mark), p.colored_name, n, max_guesses,
                                              puzzle_ ? "" : ": /game wordle to play today's Wordle too"));
            }
        }

        void lost(Player& p) {
            if (p.status == Status::Lost) {
                return;
            }
            p.status = Status::Lost;
            p.guesses = max_guesses;
            chat_.game_notice(std::format("💀 {} is out of guesses for today's Wordle!", p.colored_name));
        }

        // How today is going, as far as we heard.
        void standings() const {
            if (players_.empty()) {
                chat_.game_notice("Nobody has played today's Wordle yet.");
                return;
            }
            std::string line;
            for (const Player& p : players_) {
                line += std::format("{}{} {}", line.empty() ? "" : ", ", p.colored_name,
                                    p.status == Status::Solved ? std::format("{}/{}", p.guesses, max_guesses)
                                    : p.status == Status::Lost ? std::string("X/6")
                                                               : std::format("playing ({}/{})", p.guesses,
                                                                             max_guesses));
            }
            chat_.game_notice(std::format("Today's Wordle so far: {}", line));
        }

        // Who played a day, as the records say: "DATE.WHO.N" keys (see the top).
        static std::string who(std::uint64_t user, std::uint64_t sender) {
            return user ? std::format("{:x}", user) : std::format("s{:x}", sender);
        }

        // Keeps what a player did on a day (or tells, answering "ask"), as records.
        void remember(std::uint64_t sender, std::string_view name, std::string_view event, std::string_view date,
                      std::string_view rest) {
            const std::string player = who(chat_.user_of(sender), sender);
            const auto keep = [&](std::uint64_t n, std::string_view mark) {
                chat_.record_from(sender, name, game_name, std::format("{}.{}.{}", date, player, n),
                                  std::format("guess {} {} {}", date, n, mark));
            };
            const auto keep_lost = [&] {
                chat_.record_from(sender, name, game_name, std::format("{}.{}.x", date, player),
                                  std::format("lost {}", date));
            };
            if (event == "guess") {
                const auto n = parse_number(next_field(rest));
                const std::string_view mark = next_field(rest);
                if (n && *n >= 1 && *n <= max_guesses && mark.size() == letters &&
                    mark.find_first_not_of("gyb") == std::string_view::npos) {
                    keep(*n, mark);
                }
            } else if (event == "lost") {
                keep_lost();
            } else if (event == "result") {
                // How they did so far, without the marks: found (s) is a last guess of all greens, lost (l) is
                // lost, and still guessing (g) is a guess that was not the word.
                const auto n = parse_number(next_field(rest));
                const std::string_view state = next_field(rest);
                if (!n || *n > max_guesses || state.size() != 1) {
                    return;
                }
                if (state == "l") {
                    keep_lost();
                } else if (*n >= 1 && (state == "s" || state == "g")) {
                    keep(*n, state == "s" ? "ggggg" : "bbbbb");
                }
            }
        }

        bool settled(std::string_view date) const {
            return std::ranges::find(settled_, date) != settled_.end();
        }

        // The end of each day played that is not over here yet, due at its midnight.
        void schedule_ends() {
            for (const auto& r : chat_.records(game_name)) {
                std::string_view data = r.data;
                next_field(data);
                if (const std::string_view date = next_field(data);
                    is_date(date) && !settled(date) && end_of(date) != 0) {
                    chat_.schedule(std::format("{}{}", end_prefix, date), end_of(date));
                }
            }
        }

        // The end of a day: whoever found the word in the fewest guesses wins it. From the records, so also of a day
        // whose midnight nobody was online at, as soon as somebody is.
        void settle(const std::string& date) {
            if (!is_date(date) || settled(date)) {
                return;
            }
            settled_.push_back(date);
            while (settled_.size() > settled_kept) {
                settled_.erase(settled_.begin());
            }
            std::string list;
            for (const auto& d : settled_) {
                list += std::format("{}{}", list.empty() ? "" : ",", d);
            }
            config::set(settled_key, list);

            struct Result {
                std::uint64_t sender = 0;
                std::uint64_t user = 0;
                std::string name;
                std::uint64_t guesses = 0;
                bool solved = false;
                std::time_t time = 0;
            };
            std::map<std::string, Result> results;
            for (const auto& r : chat_.records(game_name)) {
                std::string_view data = r.data;
                const std::string_view event = next_field(data);
                if (next_field(data) != date) {
                    continue;
                }
                const auto first = r.key.find('.');
                const auto second = first == std::string::npos ? first : r.key.find('.', first + 1);
                if (second == std::string::npos) {
                    continue;
                }
                Result& res = results[r.key.substr(first + 1, second - first - 1)];
                if (r.time >= res.time) {
                    res.time = r.time;
                    res.sender = r.sender;
                    res.name = r.name;
                }
                res.user = r.user ? r.user : res.user;
                if (event == "guess") {
                    const auto n = parse_number(next_field(data));
                    const std::string_view mark = next_field(data);
                    if (!n || *n < 1 || *n > max_guesses) {
                        continue;
                    }
                    if (mark == "ggggg") {
                        if (!res.solved || *n < res.guesses) {
                            res.solved = true;
                            res.guesses = *n;
                        }
                    } else if (!res.solved) {
                        res.guesses = std::max(res.guesses, *n);
                    }
                } else if (event == "lost" && !res.solved) {
                    res.guesses = max_guesses;
                }
            }
            if (results.empty()) {
                return;
            }
            if (std::time(nullptr) > end_of(date) + late_after) {
                chat_.game_notice(std::format("🕛 Nobody was online when the Wordle of {} ended: here is how it went.",
                                              date));
            }
            std::vector<const Result*> solved;
            for (const auto& [key, res] : results) {
                if (res.solved) {
                    solved.push_back(&res);
                }
            }
            const bool known = puzzle_ && puzzle_->date == date;
            const std::string word = known ? std::format(" The word was {}.", big(puzzle_->word)) : "";
            const std::string title = known ? std::format("Wordle {}", puzzle_->id) : std::format("the Wordle of {}", date);
            if (solved.empty()) {
                chat_.game_notice(std::format("🟩 Nobody found {}.{}", title, word));
                return;
            }
            const std::uint64_t best = std::ranges::min(solved, {}, &Result::guesses)->guesses;
            std::string winners;
            std::string others;
            int tied = 0;
            for (const Result* p : solved) {
                const std::string name = chat_.colored_name(p->sender, p->name);
                if (p->guesses == best) {
                    winners += std::format("{}{}", winners.empty() ? "" : " and ", name);
                    ++tied;
                    games_.add_win(game_name, p->sender, p->name);
                } else {
                    others += std::format("{}{} {}/{}", others.empty() ? "" : ", ", name, p->guesses, max_guesses);
                }
            }
            chat_.game_notice(std::format("🏆 {} win{} {} in {}!{}{}", winners, tied == 1 ? "s" : "", title,
                                          guesses(best), word,
                                          others.empty() ? "" : std::format(" Then: {}.", others)));
            // For the ratings, by the guesses it took: those who did not find the word are last, tied. By user, as
            // the senders of then may be gone.
            std::vector<Placing> placings;
            for (const auto& [key, res] : results) {
                const std::ptrdiff_t better = !res.solved ? std::ssize(solved)
                                                          : std::ranges::count_if(solved, [&](const Result* o) {
                                                                return o->guesses < res.guesses;
                                                            });
                placings.push_back({res.sender, res.name, static_cast<int>(better), res.user});
            }
            games_.rate(game_name, placings);
        }

        // Midnight: a new Wordle (the old day ends by itself, see settle()).
        void new_day() {
            if (puzzle_ || !players_.empty()) {
                chat_.game_notice("🟩 A new Wordle is out: /game wordle to play it.");
            }
            puzzle_.reset();
            players_.clear();
            board_.clear();
            pending_.reset();
        }

        void send(std::string_view event) {
            chat_.send_game(std::format("{} {}", game_name, event));
        }

        Chat& chat_;
        Screen& terminal_;
        Games& games_;

        // Today, as a date: when it changes, it is midnight.
        std::string day_;
        // Today's word, once it came.
        std::optional<Puzzle> puzzle_;
        // Who played today, us included, as far as we heard.
        std::vector<Player> players_;
        // Our guesses and their marks.
        std::vector<std::pair<std::string, std::string>> board_;
        // A guess typed before the word came.
        std::optional<std::string> pending_;
        // The days ended here (see settle()), the oldest first; and the records seen last (see tick()).
        std::vector<std::string> settled_;
        // (None seen at first, so that the records kept from before count.)
        std::uint64_t records_seen_ = static_cast<std::uint64_t>(-1);

        // The word coming from the Times, from the fetcher's thread.
        std::mutex fetch_mutex_;
        bool fetching_ = false;
        bool failed_ = false;
        std::optional<Puzzle> fetched_;
        // Last, so that it is stopped (and curl killed) before the rest goes.
        std::jthread fetcher_;
    };

} // namespace

std::unique_ptr<Game> make_wordle(Chat& chat, Screen& terminal, Games& games) {
    return std::make_unique<Wordle>(chat, terminal, games);
}

} // namespace zchat::game
