// The hub (/hub): a 3D world everybody in the chat can walk around in, and bowl in, in a window of its own (the
// zchat_hub program, see hub/). zchat starts it, and is its line to the others: it tells the hub, in lines of text on
// its standard input, who we are, where the others are and what they say, and the hub tells zchat where we are and
// what we do (see hub/hub.cpp for the lines). zchat passes it on as Game packets:
//   hub at <x> <z> <yaw> <anim> <lane>     where we are: when it changes, and every second while we are in the hub
//   hub bowl <lane> <state...>             how our game of bowling is, while we play
//   hub leave                              we left the hub (closed its window)
// What we say in the hub is said in #general, and what anybody in the hub says in #general shows over their head.

#include "game.hpp"

#include "color.hpp"
#include "process.hpp"
#include "world_place.hpp"

#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;

    constexpr std::string_view game_name = "hub";
    constexpr auto heartbeat = 1s;
    constexpr auto timeout = 5s;
    constexpr auto prune_interval = 500ms;

    // Numbers only, and spaces: what the packets about places and lanes say (nothing that could be another line).
    bool numbers(std::string_view s) {
        return !s.empty() && s.size() < 900 &&
               s.find_first_not_of("0123456789.- ") == std::string_view::npos;
    }

    std::size_t fields(std::string_view s) {
        std::size_t n = 0;
        bool in = false;
        for (const char c : s) {
            n += c != ' ' && !in;
            in = c != ' ';
        }
        return n;
    }

    // zchat_hub, next to zchat (or where zchat was built, for one run from elsewhere).
    std::filesystem::path find_hub() {
        std::vector<std::filesystem::path> places;
#ifdef _WIN32
        wchar_t self[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, self, MAX_PATH) > 0) {
            places.push_back(std::filesystem::path(self).parent_path() / "zchat_hub.exe");
        }
#ifdef ZCHAT_BUILT_EXE
        places.push_back(std::filesystem::path(ZCHAT_BUILT_EXE).parent_path() / "zchat_hub.exe");
#endif
#endif
        for (const auto& p : places) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(p, ec)) {
                return p;
            }
        }
        return {};
    }

    class Hub : public Game {
    public:
        Hub(Chat& chat) :
            chat_(chat) {
        }

        ~Hub() override {
            if (child_) {
                child_.reset();
                send("leave");
            }
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "a 3D world to walk around in with everybody, and go bowling";
        }

        bool listed() const override {
            return false;
        }

        // /hub: its window opens.
        void start() override {
            if (child_) {
                chat_.notice("🕹️ The hub is open already: it is the window called zchat hub.");
                return;
            }
#if defined(ZCHAT_HUB) && defined(_WIN32)
            const auto path = find_hub();
            if (path.empty()) {
                chat_.notice("🕹️ The hub is not here: zchat_hub.exe should be next to zchat (/update builds it).");
                return;
            }
            std::string error;
            const auto u8 = path.u8string();
            child_ = process::Child::start(
                {std::string(u8.begin(), u8.end())},
                [this](std::string line) {
                    std::scoped_lock lock(mutex_);
                    from_hub_.push_back(std::move(line));
                },
                [this](int, std::string) {
                    std::scoped_lock lock(mutex_);
                    exited_ = true;
                },
                error);
            if (!child_) {
                chat_.notice(std::format("🕹️ Could not open the hub: {}", error));
                return;
            }
            exited_ = false;
            const Color c = color_of_id(chat_.id());
            write(std::format("me {:x} {} {} {} {}", chat_.id(), c.r, c.g, c.b, chat_.name()));
            for (const auto& [id, p] : players_) {
                write_place(id, p);
            }
            chat_.notice("🕹️ The hub is open, in a window of its own: walk around with WASD, Enter talks, and the "
                         "bowling is at the end of the path.");
#else
            chat_.notice("🕹️ The hub is a window of its own, built on Windows only for now (cmake -DZCHAT_HUB=ON).");
#endif
        }

        bool command(std::string_view args) override {
            if (args == "who") {
                tell_who();
                return true;
            }
            return false;
        }

        void receive(std::uint64_t sender, std::string_view name, std::string_view text) override {
            const auto space = text.find(' ');
            const std::string_view verb = text.substr(0, space);
            const std::string_view rest = space == std::string_view::npos ? std::string_view() : text.substr(space + 1);
            if (verb == "leave") {
                if (players_.erase(sender) > 0) {
                    write(std::format("gone {:x}", sender));
                }
            } else if (verb == "at" && numbers(rest) && fields(rest) == 5) {
                const bool fresh = !players_.contains(sender);
                Player& p = players_[sender];
                p.name = std::string(name);
                p.place = std::string(rest);
                p.seen = clock::now();
                write_place(sender, p);
                // A newcomer hears where we are right away.
                answer_ = answer_ || fresh;
            } else if (verb == "bowl" && numbers(rest)) {
                write(std::format("bowl {:x} {}", sender, rest));
            }
        }

        // Every line of #general (ours too): over the head of who said it, in the hub.
        void message(std::uint64_t sender, std::string_view name, std::string_view text) override {
            (void)name;
            if (!child_ || (sender != chat_.id() && !players_.contains(sender))) {
                return;
            }
            if (const std::string bubble = world::bubble_text(text); !bubble.empty()) {
                write(std::format("say {:x} {}", sender, bubble));
            }
        }

        void tick() override {
            std::deque<std::string> lines;
            bool exited = false;
            {
                std::scoped_lock lock(mutex_);
                lines.swap(from_hub_);
                exited = exited_;
            }
            for (const auto& line : lines) {
                from_hub(line);
            }
            if (exited && child_) {
                child_.reset();
                place_.clear();
                send("leave");
            }
            const auto now = clock::now();
            if (child_ && !place_.empty() && (now - sent_ >= heartbeat || answer_)) {
                send("at " + place_);
                sent_ = now;
                answer_ = false;
            }
            if (now >= next_prune_) {
                next_prune_ = now + prune_interval;
                std::set<std::uint64_t> online;
                for (const auto& [id, name] : chat_.people()) {
                    online.insert(id);
                }
                for (auto it = players_.begin(); it != players_.end();) {
                    if (now - it->second.seen > timeout || !online.contains(it->first)) {
                        write(std::format("gone {:x}", it->first));
                        it = players_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }

    private:
        struct Player {
            std::string name;
            std::string place;
            clock::time_point seen;
        };

        void send(std::string_view event) {
            chat_.send_game(std::format("{} {}", game_name, event));
        }

        void write(const std::string& line) {
            if (child_) {
                child_->write(line);
            }
        }

        void write_place(std::uint64_t id, const Player& p) {
            const Color c = color_of_id(id);
            write(std::format("at {:x} {} {} {} {} {}", id, c.r, c.g, c.b, p.place, p.name));
        }

        // A line of the hub's.
        void from_hub(const std::string& line) {
            const auto space = line.find(' ');
            const std::string verb = line.substr(0, space);
            const std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);
            if (verb == "at" && numbers(rest) && fields(rest) == 5) {
                if (rest != place_) {
                    place_ = rest;
                    send("at " + place_);
                    sent_ = clock::now();
                }
            } else if (verb == "bowl" && numbers(rest)) {
                send("bowl " + rest);
            } else if (verb == "say" && !rest.empty()) {
                // (Not from here: saying calls the games' hooks, this is one.)
                chat_.say_soon(rest);
            }
        }

        void tell_who() const {
            std::string list = child_ ? chat_.colored_own_name() + " (you)" : std::string();
            for (const auto& [id, p] : players_) {
                list += (list.empty() ? "" : ", ") + chat_.colored_name(id, p.name);
            }
            chat_.notice(list.empty() ? std::string("🕹️ Nobody is in the hub now.") : "🕹️ In the hub: " + list);
        }

        Chat& chat_;
        std::unique_ptr<process::Child> child_;

        // What the hub said, and whether it ended, from its thread.
        std::mutex mutex_;
        std::deque<std::string> from_hub_;
        bool exited_ = false;

        // Where we are (as the hub says), told every second; and whether a newcomer should hear it now.
        std::string place_;
        clock::time_point sent_;
        bool answer_ = false;

        // The others in the hub, by sender id.
        std::map<std::uint64_t, Player> players_;
        clock::time_point next_prune_;
    };

} // namespace

std::unique_ptr<Game> make_hub(Chat& chat, Screen& terminal, Games& games) {
    (void)terminal;
    (void)games;
    return std::make_unique<Hub>(chat);
}

} // namespace zchat::game
