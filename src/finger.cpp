// Fastest finger: a phrase shows up for everyone, and the first to type it exactly wins.
//
// The one who starts a round is its referee. It sends, as Game packets:
//   finger ready <round>                       a phrase comes in a few seconds
//   finger go <round> <phrase>                 type it!
//   finger win <round> <id> <ms> <name>        the first message with the phrase the referee got, after ms
//   finger none <round> <phrase>               nobody typed it in time
// where <round> is a random hex number naming the round, and <id> the winner's sender id, in hex.

#include "game.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <optional>
#include <random>

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
                if (event == "win") {
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
            if (!referee_ || stage_ != Stage::Go || text != phrase_) {
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
                phrase_.clear();
                for (std::size_t i = 0; i < phrase_words; ++i) {
                    phrase_ += i == 0 ? "" : " ";
                    phrase_ += words[std::uniform_int_distribution<std::size_t>(0, words.size() - 1)(rng_)];
                }
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
        std::string phrase_;
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
