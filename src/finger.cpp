// Fastest finger: a phrase shows up for everyone, and the first to type it exactly wins.
//
// So that it cannot just be copied and pasted, the phrase shown has a letter of each word swapped for one that looks
// the same from another alphabet (a Latin 'a' for a Cyrillic 'а'): typed on a keyboard it matches, pasted it does
// not, and the one who pasted it is out of the round.
//
// The one who starts a round is its referee. It sends, as Game packets:
//   finger ready <round>                       a phrase comes in a few seconds
//   finger go <round> <phrase>                 type it!
//   finger win <round> <id> <ms> <name>        the first message with the phrase the referee got, after ms
//   finger paste <round> <id> <name>           someone pasted the phrase shown, and is out of the round
//   finger none <round> <phrase>               nobody typed it in time
// where <round> is a random hex number naming the round, and <id> the winner's sender id, in hex.

#include "game.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <optional>
#include <random>
#include <vector>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;

    constexpr std::string_view game_name = "finger";
    // From "ready" to "go": time to get the hands on the keyboard.
    constexpr auto countdown = 3s;
    constexpr auto answer_time = 30s;
    // How much longer the players wait for the referee's result, before thinking it left.
    constexpr auto patience = 5s;
    constexpr std::size_t phrase_words = 3;

    constexpr std::array words = std::to_array<std::string_view>({
        "banana",   "rocket",  "pickle",    "wizard",   "volcano",   "pancake",     "noodle",   "penguin",  "thunder",
        "cactus",   "donut",   "jellyfish", "kazoo",    "llama",     "marshmallow", "ninja",    "octopus",  "pirate",
        "quokka",   "raccoon", "spaghetti", "tornado",  "unicorn",   "waffle",      "yeti",     "zombie",   "avocado",
        "bubble",   "cookie",  "dragon",    "espresso", "falafel",   "giraffe",     "hamster",  "igloo",    "jazz",
        "koala",    "lasagna", "mango",     "narwhal",  "origami",   "potato",      "quiz",     "robot",    "sushi",
        "taco",     "ukulele", "vampire",   "walrus",   "xylophone", "yogurt",      "zucchini", "asteroid", "burrito",
        "crayon",   "disco",   "eggplant",  "flamingo", "goblin",    "hiccup",      "iguana",   "jackpot",  "kiwi",
        "lemonade", "muffin",  "nugget",    "ostrich",  "pretzel",   "squid",       "toaster",  "umbrella", "velcro",
        "wombat",   "yodel",   "zeppelin",  "blizzard", "chimney",   "dolphin",     "fizzy",    "gecko",    "hedgehog",
        "jumbo",    "ketchup", "lobster",   "mustache", "pajamas",   "quack",       "sprinkle", "turbo",    "wobbly",
        "sneaky",   "grumpy",  "sparkly",   "fluffy",   "cosmic",    "spicy",       "soggy",    "squishy",  "bouncy",
    });

    // Letters and the ones from other alphabets that look the same, in most fonts: Cyrillic ones.
    struct Lookalike {
        char latin;
        std::string_view other;
    };
    constexpr std::array lookalikes = std::to_array<Lookalike>({
        {'a', "\u0430"},
        {'c', "\u0441"},
        {'e', "\u0435"},
        {'i', "\u0456"},
        {'j', "\u0458"},
        {'o', "\u043e"},
        {'p', "\u0440"},
        {'s', "\u0455"},
        {'x', "\u0445"},
        {'y', "\u0443"},
    });

    // The text with the lookalike letters back to the Latin ones.
    std::string unswap(std::string_view text) {
        std::string out(text);
        for (const auto& l : lookalikes) {
            for (auto at = out.find(l.other); at != std::string::npos; at = out.find(l.other, at + 1)) {
                out.replace(at, l.other.size(), 1, l.latin);
            }
        }
        return out;
    }

    // Every word needs a letter to swap, or it could be pasted.
    constexpr bool all_swappable() {
        return std::ranges::all_of(words, [](std::string_view word) {
            return std::ranges::any_of(word, [](char c) {
                return std::ranges::find(lookalikes, c, &Lookalike::latin) != lookalikes.end();
            });
        });
    }
    static_assert(all_swappable(), "every word needs one of the letters in lookalikes");

    std::optional<std::uint64_t> parse_hex(std::string_view s) {
        std::uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, 16);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size() || value == 0) {
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

    class Finger final : public Game {
    public:
        Finger(Chat& chat, Terminal& terminal, Games& games) :
            chat_(chat),
            terminal_(terminal),
            games_(games),
            rng_(std::random_device {}()) {
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "fastest finger: some words show up, the first to type them exactly wins";
        }

        void start() override {
            if (stage_ != Stage::Idle) {
                chat_.notice(stage_ == Stage::Ready
                                 ? "A round of fastest finger is about to start: get ready!"
                                 : std::format("A round of fastest finger is on: type {}", bold(phrase_)));
                return;
            }
            do {
                round_ = rng_();
            } while (round_ == 0);
            referee_ = true;
            stage_ = Stage::Ready;
            deadline_ = clock::now() + countdown;
            referee_name_ = chat_.colored_own_name();
            send(std::format("ready {:x}", round_));
            announce_ready();
        }

        void receive(std::uint64_t sender, std::string_view name, std::string_view text) override {
            const std::string_view event = next_field(text);
            const auto round = parse_hex(next_field(text));
            if (!round) {
                return;
            }
            if (event == "ready") {
                // Two rounds started at the same time: everybody plays the one with the lowest number.
                if (stage_ == Stage::Idle || (stage_ == Stage::Ready && *round < round_)) {
                    join(*round, sender, name, Stage::Ready, countdown + answer_time + patience);
                    announce_ready();
                    terminal_.bell();
                }
            } else if (event == "go") {
                if (text.empty() || (stage_ != Stage::Idle && (*round != round_ || referee_))) {
                    return;
                }
                // Without the "ready" (we just arrived, or it got lost), the round is joined now.
                const bool late = stage_ == Stage::Idle;
                join(*round, sender, name, Stage::Go, answer_time + patience);
                phrase_ = std::string(text);
                announce_go();
                if (late) {
                    terminal_.bell();
                }
            } else if (*round == round_ && !referee_ && stage_ != Stage::Idle) {
                if (event == "paste") {
                    if (const auto id = parse_hex(next_field(text)); id && !text.empty()) {
                        announce_paste(*id, text);
                    }
                } else if (event == "win") {
                    const auto id = parse_hex(next_field(text));
                    const std::string_view ms = next_field(text);
                    long long millis = 0;
                    if (!id || std::from_chars(ms.data(), ms.data() + ms.size(), millis).ptr != ms.data() + ms.size() ||
                        text.empty()) {
                        return;
                    }
                    announce_win(*id, text, std::chrono::milliseconds(millis));
                    stage_ = Stage::Idle;
                } else if (event == "none") {
                    if (!text.empty()) {
                        phrase_ = std::string(text);
                    }
                    announce_none();
                    stage_ = Stage::Idle;
                }
            }
        }

        void message(std::uint64_t sender, std::string_view name, std::string_view text) override {
            if (!referee_ || stage_ != Stage::Go || std::ranges::find(out_, sender) != out_.end()) {
                return;
            }
            if (text != answer_) {
                // The phrase shown, lookalike letters and all: copied from the screen.
                if (text != unswap(text) && unswap(text) == answer_) {
                    out_.push_back(sender);
                    send(std::format("paste {:x} {:x} {}", round_, sender, name));
                    announce_paste(sender, name);
                }
                return;
            }
            const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - started_);
            send(std::format("win {:x} {:x} {} {}", round_, sender, time.count(), name));
            announce_win(sender, name, time);
            stage_ = Stage::Idle;
        }

        void tick() override {
            if (stage_ == Stage::Idle || clock::now() < deadline_) {
                return;
            }
            if (!referee_) {
                chat_.notice(std::format("The round of fastest finger by {} ended without a result: they may have "
                                         "left.",
                                         referee_name_));
                stage_ = Stage::Idle;
            } else if (stage_ == Stage::Ready) {
                make_phrase();
                stage_ = Stage::Go;
                started_ = clock::now();
                deadline_ = started_ + answer_time;
                send(std::format("go {:x} {}", round_, phrase_));
                announce_go();
            } else {
                send(std::format("none {:x} {}", round_, phrase_));
                announce_none();
                stage_ = Stage::Idle;
            }
        }

    private:
        enum class Stage {
            Idle,
            Ready, // waiting for the phrase
            Go,    // waiting for someone to type it
        };

        // Plays a round refereed by someone else; if nothing is heard of it for so long, it is given up.
        void join(std::uint64_t round, std::uint64_t referee, std::string_view referee_name, Stage stage,
                  clock::duration wait) {
            round_ = round;
            referee_ = false;
            stage_ = stage;
            phrase_.clear();
            deadline_ = clock::now() + wait;
            referee_name_ = chat_.colored_name(referee, referee_name);
        }

        // Picks the words to type, and the way they are shown.
        void make_phrase() {
            answer_.clear();
            phrase_.clear();
            out_.clear();
            for (std::size_t i = 0; i < phrase_words; ++i) {
                const std::string_view word =
                    words[std::uniform_int_distribution<std::size_t>(0, words.size() - 1)(rng_)];
                answer_ += i == 0 ? "" : " ";
                answer_ += word;
                // One letter of each word, so that pasting any of them is caught.
                std::vector<std::size_t> swappable;
                for (std::size_t at = 0; at < word.size(); ++at) {
                    if (std::ranges::find(lookalikes, word[at], &Lookalike::latin) != lookalikes.end()) {
                        swappable.push_back(at);
                    }
                }
                phrase_ += i == 0 ? "" : " ";
                const std::size_t at =
                    swappable[std::uniform_int_distribution<std::size_t>(0, swappable.size() - 1)(rng_)];
                phrase_ += word.substr(0, at);
                phrase_ += std::ranges::find(lookalikes, word[at], &Lookalike::latin)->other;
                phrase_ += word.substr(at + 1);
            }
        }

        void send(std::string_view event) {
            chat_.send_game(std::format("{} {}", game_name, event));
        }

        std::string bold(std::string_view text) const {
            return terminal_.colors() ? std::format("\x1b[1m{}\x1b[0m", text) : std::string(text);
        }

        void announce_ready() {
            chat_.notice(std::format("⌨️  Fastest finger, started by {}! Some words show up in {} seconds: the first to "
                                     "type them exactly wins.",
                                     referee_name_, countdown.count()));
        }

        void announce_go() {
            chat_.notice(std::format("GO! Type: {}", bold(phrase_)));
        }

        void announce_win(std::uint64_t id, std::string_view name, std::chrono::milliseconds time) {
            const int wins = games_.add_win(game_name, id, name);
            chat_.notice(std::format("🏆 {} wins fastest finger in {:.1f} seconds!{}", chat_.colored_name(id, name),
                                     static_cast<double>(time.count()) / 1000,
                                     wins > 1 ? std::format(" That's {} wins.", wins) : ""));
        }

        void announce_paste(std::uint64_t id, std::string_view name) {
            chat_.notice(std::format("📋 {} pasted the words instead of typing them: out of this round!",
                                     chat_.colored_name(id, name)));
        }

        void announce_none() {
            chat_.notice(std::format("⏰ Time's up! Nobody typed {}", bold(phrase_)));
        }

        Chat& chat_;
        Terminal& terminal_;
        Games& games_;
        std::mt19937_64 rng_;

        Stage stage_ = Stage::Idle;
        // Whether we started the current round, and so tell the others what happens in it.
        bool referee_ = false;
        std::uint64_t round_ = 0;
        std::string referee_name_; // colored
        // As shown, with lookalike letters.
        std::string phrase_;
        // For the referee: the phrase as typed, and who is out of the round for pasting it.
        std::string answer_;
        std::vector<std::uint64_t> out_;
        // When the stage ends: the phrase shows up, or nobody typed it (for the others, when they stop waiting).
        clock::time_point deadline_;
        // When the phrase showed up, for the referee.
        clock::time_point started_;
    };

} // namespace

std::unique_ptr<Game> make_finger(Chat& chat, Terminal& terminal, Games& games) {
    return std::make_unique<Finger>(chat, terminal, games);
}

} // namespace zchat::game
