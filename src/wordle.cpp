// Wordle: the New York Times' puzzle of the day, played by the whole chat. Everyone guesses the same five-letter word
// on their own, with /game wordle WORD, and sees which letters are in the word; the others only see the colors, as
// when sharing a Wordle. Whoever finds it in the fewest guesses wins (the fastest, between equals).
//
// As on the Times, there is one Wordle a day, and it is played once: our guesses are saved in the config file, as
// "wordle=<date> <word>,<word>..." and then " done" once found or out of guesses. A round of the same day goes on from
// them, and a done one cannot be played again: /game wordle says to come back tomorrow.
//
// The word comes from the New York Times: https://www.nytimes.com/svc/wordle/v2/YYYY-MM-DD.json gives the puzzle of
// that day ({"id":829,"solution":"river","print_date":"2026-09-30",...}), fetched with curl, which Windows 10 and later,
// macOS and most Linux have. /game wordle plays today's.
//
// Everyone plays with their own clock after the start. The one who starts a round sends, as a Game packet:
//   wordle start <round> <puzzle> <date> <code>  a round starts, with the word shifted by the round (not shown as it is)
// and every player sends:
//   wordle guess <round> <n> <marks> <ms>        guess n (from 1), its marks (g: right place, y: in the word, b: not in
//                                                it), ms after the start: "ggggg" found the word
//   wordle lost <round>                          no guesses left
// where <round> is a random hex number naming the round. Everybody ends the round on their own clock, when time is up
// or when all the players are done and nobody has joined for a while, and shows who won from what they heard.

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
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;

    constexpr std::string_view game_name = "wordle";
    constexpr std::size_t letters = 5;
    constexpr std::uint64_t max_guesses = 6;
    constexpr auto round_time = 5min;
    // The round is not over before this, even when all the players are done, so that others can still join.
    constexpr auto join_time = 60s;
    constexpr auto warning_time = 30s;
    constexpr auto fetch_timeout = 15s;
    constexpr std::string_view saved_key = "wordle";

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

    std::string plural(std::uint64_t n, std::string_view word) {
        return std::format("{} {}{}", n, word, n == 1 ? "" : "s");
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

    // The word as sent, shifted letter by letter by the round, so that it does not show in the packets as it is.
    std::string shift(std::string_view word, std::uint64_t round, bool back) {
        std::string out(word);
        for (std::size_t i = 0; i < out.size(); ++i) {
            const int by = static_cast<int>((round >> (i * 5)) % 26);
            out[i] = static_cast<char>('a' + (out[i] - 'a' + (back ? 26 - by : by)) % 26);
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
            rng_(std::random_device {}()) {
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "the New York Times' Wordle of the day: guess the five-letter word in six tries, in the fewest wins";
        }

        void start() override {
            fetch(today());
        }

        void receive(std::uint64_t sender, std::string_view name, std::string_view text) override {
            const std::string_view event = next_field(text);
            const auto round = parse_number(next_field(text), 16);
            if (!round || *round == 0) {
                return;
            }
            if (event == "start") {
                const auto puzzle = parse_number(next_field(text));
                const std::string_view date = next_field(text);
                const std::string word = shift(next_field(text), *round, true);
                if (!puzzle || date.empty() || !is_word(word)) {
                    return;
                }
                // Two rounds started at the same time: everybody plays the one with the lowest number, if nobody
                // has guessed yet.
                if (!on_ || (*round < round_ && players_.empty())) {
                    begin(*round, {*puzzle, std::string(date), word}, chat_.colored_name(sender, name));
                    terminal_.bell();
                }
                return;
            }
            if (!on_ || *round != round_) {
                return;
            }
            if (event == "guess") {
                const auto n = parse_number(next_field(text));
                const std::string_view mark = next_field(text);
                const auto ms = parse_number(next_field(text));
                if (n && ms && *n >= 1 && *n <= max_guesses && mark.size() == letters &&
                    mark.find_first_not_of("gyb") == std::string_view::npos) {
                    guessed(player(sender, name), *n, mark, std::chrono::milliseconds(*ms));
                }
            } else if (event == "lost") {
                lost(player(sender, name));
            }
        }

        void message(std::uint64_t, std::string_view, std::string_view) override {
        }

        void tick() override {
            if (const auto puzzle = fetched()) {
                if (on_) {
                    // Someone else's round started while the word came.
                    chat_.notice("A round of Wordle is on already: /game wordle WORD to guess.");
                } else {
                    std::uint64_t round = 0;
                    do {
                        round = rng_();
                    } while (round == 0);
                    begin(round, *puzzle, chat_.colored_own_name());
                    send(std::format("start {:x} {} {} {}", round_, puzzle_.id, puzzle_.date,
                                     shift(puzzle_.word, round_, false)));
                }
            }
            if (!on_) {
                return;
            }
            const auto now = clock::now();
            if (playing() && !warned_ && now >= started_ + round_time - warning_time) {
                warned_ = true;
                chat_.notice(std::format("⏳ {} seconds left to find the Wordle!", warning_time.count()));
                terminal_.bell();
            }
            const bool all_done = std::ranges::none_of(players_, [](const Player& p) {
                return p.status == Status::Guessing;
            });
            if (now >= started_ + round_time || (!players_.empty() && all_done && now >= started_ + join_time)) {
                end();
            }
        }

        bool command(std::string_view args) override {
            // Any one word is a guess, so that a wrong one is told why.
            if (args.find(' ') == std::string_view::npos) {
                guess(args);
                return true;
            }
            return false;
        }

        std::string_view commands() const override {
            return "WORD";
        }

    private:
        enum class Status { Guessing, Solved, Lost };

        struct Player {
            std::uint64_t id = 0;
            std::string name;
            std::string colored_name;
            Status status = Status::Guessing;
            std::uint64_t guesses = 0;
            std::chrono::milliseconds time {};
        };

        // Asks the Times for the puzzle of a day, in the background: tick() starts the round when it comes.
        void fetch(std::chrono::year_month_day day) {
            if (on_) {
                chat_.notice(playing() ? "A round of Wordle is on: /game wordle WORD to guess."
                                       : "A round of Wordle is on: /game wordle WORD to join it!");
                return;
            }
            const std::string date = iso_date(day);
            if (saved(date).done) {
                played_already(date);
                return;
            }
            std::scoped_lock lock(fetch_mutex_);
            if (fetching_) {
                chat_.notice("The Wordle is on its way from the New York Times…");
                return;
            }
            fetching_ = true;
            chat_.notice(std::format("📰 Getting the Wordle of {} from the New York Times…", date));
            if (fetcher_.joinable()) {
                fetcher_.join();
            }
            fetcher_ = std::jthread([this, date](std::stop_token stop) {
                const auto result =
                    process::run({"curl", "-s", "-f", "-L", "-m", "10",
                                  std::format("https://www.nytimes.com/svc/wordle/v2/{}.json", date)},
                                 stop, fetch_timeout);
                Puzzle puzzle;
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
                    puzzle = {id.value_or(0), date, word};
                    fetched_ = puzzle;
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
                chat_.notice("Could not get the Wordle from the New York Times: is curl installed, and is there "
                             "internet?");
            }
            if (!fetched_) {
                return std::nullopt;
            }
            fetching_ = false;
            return std::exchange(fetched_, std::nullopt);
        }

        void begin(std::uint64_t round, Puzzle puzzle, std::string referee_name) {
            on_ = true;
            round_ = round;
            puzzle_ = std::move(puzzle);
            started_ = clock::now();
            players_.clear();
            me_.reset();
            done_ = false;
            board_.clear();
            warned_ = false;
            restore();
            chat_.notice(std::format("🟩 Wordle {} ({}), started by {}! Guess the five-letter word with /game wordle "
                                     "WORD: you see which letters are in it, the others only see your colors.",
                                     puzzle_.id, puzzle_.date, referee_name));
            chat_.notice(std::format("   {} guesses each; whoever finds it in the fewest wins (the fastest, between "
                                     "equals). {} minutes to play.",
                                     max_guesses,
                                     std::chrono::duration_cast<std::chrono::minutes>(round_time).count()));
        }

        bool playing() const {
            const auto it = me_ ? std::ranges::find(players_, *me_, &Player::id) : players_.end();
            return it != players_.end() && it->status == Status::Guessing;
        }

        // The player, added if we had not heard of them (they just joined, or it is us).
        Player& player(std::uint64_t id, std::string_view name) {
            auto it = std::ranges::find(players_, id, &Player::id);
            if (it == players_.end()) {
                players_.push_back({id, std::string(name), chat_.colored_name(id, name)});
                it = players_.end() - 1;
            }
            return *it;
        }

        static int rank(char mark) {
            return mark == 'g' ? 2 : mark == 'y' ? 1 : 0;
        }

        struct Saved {
            std::vector<std::string> words;
            bool done = false;
        };

        // Our guesses of the Wordle of a day, as saved; none for another day.
        static Saved saved(std::string_view date) {
            Saved out;
            const auto value = config::get(saved_key);
            if (!value || !value->starts_with(date) || value->size() <= date.size() || (*value)[date.size()] != ' ') {
                return out;
            }
            std::string_view rest = std::string_view(*value).substr(date.size() + 1);
            if (rest.ends_with(" done")) {
                out.done = true;
                rest.remove_suffix(5);
            }
            while (!rest.empty() && out.words.size() < max_guesses) {
                const auto comma = rest.find(',');
                if (const std::string_view word = rest.substr(0, comma); is_word(word)) {
                    out.words.emplace_back(word);
                }
                rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 1);
            }
            return out;
        }

        void save(bool done) const {
            std::string words;
            for (const auto& [word, mark] : board_) {
                words += std::format("{}{}", words.empty() ? "" : ",", word);
            }
            if (!config::set(saved_key, std::format("{} {}{}", puzzle_.date, words, done ? " done" : ""))) {
                chat_.notice(std::format("Could not save your Wordle to {}", config::file().string()));
            }
        }

        void played_already(std::string_view date) const {
            chat_.notice(std::format("You played the Wordle of {} already: there is one a day, come back tomorrow!",
                                     date));
        }

        // A round of a day we played already: goes on from our saved guesses (or we just watch, when done).
        void restore() {
            const Saved s = saved(puzzle_.date);
            for (const std::string& word : s.words) {
                board_.push_back({word, marks(word, puzzle_.word)});
            }
            if (board_.empty() && !s.done) {
                return;
            }
            // Not among the players, so that the round does not wait for us, nor count us again.
            me_ = chat_.id();
            if (s.done || board_.back().second == "ggggg" || board_.size() == max_guesses) {
                done_ = true;
                chat_.notice("You played today's Wordle already: you can watch the others.");
                return;
            }
            Player& me = player(*me_, chat_.name());
            me.guesses = board_.size();
            chat_.notice(std::format("You started this Wordle earlier: {} left.",
                                     plural(max_guesses - board_.size(), "guess")));
            draw();
        }

        void guess(std::string_view word) {
            if (!on_) {
                std::scoped_lock lock(fetch_mutex_);
                chat_.notice(fetching_ ? "The Wordle is on its way from the New York Times…"
                                       : "No round of Wordle is on: /game wordle starts one.");
                return;
            }
            if (!is_word(word)) {
                chat_.notice("A guess is a word of five letters, from a to z.");
                return;
            }
            // The Times' word always counts, even if the list does not have it.
            if (word != puzzle_.word && !std::ranges::binary_search(dictionary(), word)) {
                chat_.notice(std::format("{} is not in the word list: it does not count as a guess.", big(word)));
                return;
            }
            if (done_ || (me_ && player(*me_, chat_.name()).status != Status::Guessing)) {
                played_already(puzzle_.date);
                return;
            }
            me_ = chat_.id();
            Player& me = player(*me_, chat_.name());
            const std::string mark = marks(word, puzzle_.word);
            board_.push_back({std::string(word), mark});
            const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - started_);
            send(std::format("guess {:x} {} {} {}", round_, board_.size(), mark, time.count()));
            draw();
            guessed(me, board_.size(), mark, time);
            if (me.status == Status::Guessing && board_.size() == max_guesses) {
                chat_.notice(std::format("No guesses left! The word was {}.", big(puzzle_.word)));
                send(std::format("lost {:x}", round_));
                lost(me);
            }
            save(me.status != Status::Guessing);
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
            out += "\n\n     ";
            for (std::string_view row : {"qwertyuiop", "asdfghjkl", "zxcvbnm"}) {
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
                out += row.size() == 10 ? "\n      " : "\n        ";
            }
            terminal_.print(out);
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

        void guessed(Player& p, std::uint64_t n, std::string_view mark, std::chrono::milliseconds time) {
            if (p.status != Status::Guessing || n <= p.guesses) {
                return;
            }
            p.guesses = n;
            p.time = time;
            if (mark == "ggggg") {
                p.status = Status::Solved;
                chat_.notice(std::format("🎉 {} found the Wordle in {} ({}/{})!", p.colored_name,
                                         plural(n, "guess"), n, max_guesses));
            } else if (!me_ || p.id != *me_) {
                chat_.notice(std::format("{} {} ({}/{})", emoji(mark), p.colored_name, n, max_guesses));
            }
        }

        void lost(Player& p) {
            if (p.status != Status::Guessing) {
                return;
            }
            p.status = Status::Lost;
            chat_.notice(std::format("💀 {} is out of guesses!", p.colored_name));
        }

        void end() {
            on_ = false;
            const std::string word = big(puzzle_.word);
            if (players_.empty()) {
                chat_.notice(std::format("🟩 Nobody played that Wordle. The word was {}.", word));
                return;
            }
            std::vector<const Player*> ranking;
            for (const Player& p : players_) {
                if (p.status == Status::Solved) {
                    ranking.push_back(&p);
                }
            }
            std::ranges::stable_sort(ranking, [](const Player* a, const Player* b) {
                return a->guesses != b->guesses ? a->guesses < b->guesses : a->time < b->time;
            });
            if (ranking.empty()) {
                chat_.notice(std::format("🟩 Wordle is over: nobody found {}.", word));
                return;
            }
            const Player& winner = *ranking.front();
            const int wins = games_.add_win(game_name, winner.id, winner.name);
            std::string others;
            for (std::size_t i = 1; i < ranking.size(); ++i) {
                others += std::format("{}{} {}/{}", others.empty() ? "" : ", ", ranking[i]->colored_name,
                                      ranking[i]->guesses, max_guesses);
            }
            chat_.notice(std::format("🏆 {} wins the Wordle {} in {} ({:.1f} seconds)! The word was {}.{}{}",
                                     winner.colored_name, puzzle_.id, plural(winner.guesses, "guess"),
                                     static_cast<double>(winner.time.count()) / 1000, word,
                                     others.empty() ? "" : std::format(" Then: {}.", others),
                                     wins > 1 ? std::format(" That's {} wins.", wins) : ""));
        }

        void send(std::string_view event) {
            chat_.send_game(std::format("{} {}", game_name, event));
        }

        Chat& chat_;
        Screen& terminal_;
        Games& games_;
        std::mt19937_64 rng_;

        bool on_ = false;
        std::uint64_t round_ = 0;
        Puzzle puzzle_;
        clock::time_point started_;
        std::vector<Player> players_;
        // Our id among the players, once we play the round.
        std::optional<std::uint64_t> me_;
        // Our guesses and their marks.
        std::vector<std::pair<std::string, std::string>> board_;
        // We played this day's Wordle to its end before the round: we only watch it.
        bool done_ = false;
        bool warned_ = false;

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
