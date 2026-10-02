// Spy: /spy @NAME asks NAME to let you see their screen(s). Nothing is captured behind anyone's back — the person at
// the other machine is asked first, and a screenshot is taken only if they agree with /spy allow. If they /spy deny,
// or do nothing before the time is up, no picture is ever taken. Asking costs coins (see coins.hpp); the asker gets
// half of them back when the peek does not happen (a refusal, no answer, or a machine that cannot take a screenshot),
// and the person who agrees earns the same half for being a good sport.
//
// When someone agrees, THEIR zchat takes the picture of each of their monitors (see capture.cpp) and sends them to
// the chat like any other picture, so the screens are shared the usual way: zchat never reaches into another machine.
//
// Not a game, but it travels the same way as /kick: as Game packets whose text starts with "spy".
//   spy ask  <round> <target> <price> <seconds> <name>   who asks (the sender), of whom (<target>, their <name>),
//                                                         how much they paid, and how long the target has to answer
//   spy done <round> <target> shot|denied|failed|gone <count> <name>   the outcome, from the target's zchat: the
//                                                         screens were shared (shot, <count> of them), refused
//                                                         (denied), could not be taken (failed), or the target left
//                                                         (gone); sent a few times so the asker gets it
// where <round> is a random hex number naming the request and <target> a sender id, in hex. The pictures themselves
// are ordinary Images, sent by the target right before the "done shot".

#include "capture.hpp"
#include "coins.hpp"
#include "game.hpp"
#include "image.hpp"

#include "text.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;

    constexpr std::string_view game_name = "spy";
    // How long the target has to answer before the request lapses with no picture taken.
    constexpr auto answer_time = 30s;
    // How much longer the asker waits for the outcome before giving up and taking the refund.
    constexpr auto patience = 25s;
    // How often the outcome is sent again, and how many times, so the asker gets it.
    constexpr auto resend_every = 2s;
    constexpr int done_sends = 3;
    // How wide and tall, in pixels, each screen is sent (see image::encode_picture()).
    constexpr int screen_pixels = 1920;

    std::string lowercase(std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        return out;
    }

    std::string trim(std::string_view s) {
        while (!s.empty() && s.front() == ' ') {
            s.remove_prefix(1);
        }
        while (!s.empty() && s.back() == ' ') {
            s.remove_suffix(1);
        }
        return std::string(s);
    }

    // A name as typed: the window's '@' list types it after an '@', which is not part of it.
    std::string person(std::string_view s) {
        std::string out = trim(s);
        return out.starts_with('@') ? trim(std::string_view(out).substr(1)) : out;
    }

    std::optional<std::uint64_t> parse_number(std::string_view s, int base) {
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

    std::string screens_of(std::size_t n) {
        return std::format("{} screen{}", n, n == 1 ? "" : "s");
    }

    class Spy final : public Game {
    public:
        Spy(Chat& chat, Screen& terminal, std::function<bool(long long)> can_afford,
            std::function<bool(std::string_view)> spend, std::function<void(long long)> cash) :
            chat_(chat),
            terminal_(terminal),
            can_afford_(std::move(can_afford)),
            spend_(std::move(spend)),
            cash_(std::move(cash)),
            rng_(std::random_device {}()) {
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "peek at someone's screen, if they let you";
        }

        bool listed() const override {
            return false;
        }

        bool rated() const override {
            return false;
        }

        bool keeps_case() const override {
            return true;
        }

        // /spy alone: how it works, and the requests on.
        void start() override {
            const long long price = coins::price("spy").value_or(0);
            chat_.notice("/spy @NAME asks NAME to let you see their screen(s). They decide: /spy allow shares them, "
                         "/spy deny refuses, and doing nothing refuses too. Nothing is captured without their yes.");
            chat_.notice(std::format("   Asking is free: only if they accept do you pay {} coins, and they get {} "
                                     "for letting you in (/shop).",
                                     price, half_of(price)));
            for (const Request& r : requests_) {
                if (r.us) {
                    chat_.notice(std::format("   {} wants to see your screen(s): /spy allow or /spy deny.",
                                             r.other_name));
                } else if (r.ours) {
                    chat_.notice(std::format("   Waiting for {} to answer your peek.", r.other_name));
                }
            }
        }

        // /spy @NAME, or /spy allow|deny [NAME] while a request about us is on.
        bool command(std::string_view args) override {
            std::string_view rest = args;
            const std::string choice = lowercase(next_field(rest));
            const bool yes = choice == "allow" || choice == "yes" || choice == "ok" || choice == "y";
            const bool no = choice == "deny" || choice == "no" || choice == "dodge" || choice == "n";
            // Someone could be called "allow": it is an answer only when there is a request to answer.
            if ((yes || no) && std::ranges::any_of(requests_, [](const Request& r) {
                    return r.us && !r.answered;
                })) {
                answer(yes, person(rest));
            } else {
                ask(person(args));
            }
            return true;
        }

        void receive(std::uint64_t sender, std::string_view name, std::string_view text) override {
            const std::string_view event = next_field(text);
            const auto round = parse_number(next_field(text), 16);
            if (!round || *round == 0) {
                return;
            }
            if (event == "ask") {
                receive_ask(sender, name, *round, text);
            } else if (event == "done") {
                receive_done(*round, text);
            }
        }

        void message(std::uint64_t, std::string_view, std::string_view) override {}

        void tick() override {
            const auto now = clock::now();
            for (auto it = requests_.begin(); it != requests_.end();) {
                Request& r = *it;
                if (r.us && !r.answered && now >= r.deadline) {
                    // No answer in time: as good as a yes, so the picture is taken (only /spy deny refuses).
                    r.answered = true;
                    const std::uint64_t round = r.round;
                    const std::string other_name = r.other_name;
                    const long long bounty = half_of(r.price);
                    chat_.notice(std::format("📸 You did not answer {}'s peek in time, so it is on: taking the "
                                             "picture now...",
                                             other_name));
                    done_.push_back(round);
                    it = requests_.erase(it);
                    // Capturing runs outside tools and can take a moment: on a thread of its own, like /spy allow.
                    start_capture(round, other_name, bounty);
                    continue;
                }
                if (r.ours && now >= r.deadline) {
                    chat_.notice(std::format("🕶 No answer from {}: your peek is off, and it cost you nothing.",
                                             r.other_name));
                    done_.push_back(r.round);
                    it = requests_.erase(it);
                    continue;
                }
                ++it;
            }
            for (auto it = dones_.begin(); it != dones_.end();) {
                if (now < it->next) {
                    ++it;
                    continue;
                }
                send(it->text);
                it->next = now + resend_every;
                if (--it->left == 0) {
                    it = dones_.erase(it);
                } else {
                    ++it;
                }
            }
            while (done_.size() > 64) {
                done_.pop_front();
            }
            // Let go of the capture threads that have finished (their jthread is joined as it is erased).
            std::erase_if(workers_, [](const Worker& w) {
                return w.done->load();
            });
        }

    private:
        struct Request {
            std::uint64_t round = 0;
            // We asked (we are waiting for the screens).
            bool ours = false;
            // We are the one asked (we decide).
            bool us = false;
            std::uint64_t other = 0; // the other party's id (the target for us-askers, the asker for us-targets)
            std::string other_name;  // colored
            std::string other_plain;
            long long price = 0;
            bool answered = false;  // the target decided (allow/deny), or we-the-asker got the outcome
            clock::time_point deadline;
        };

        struct Resend {
            std::string text;
            int left = 0;
            clock::time_point next;
        };

        long long half_of(long long price) const {
            return (price + 1) / 2;
        }

        // /spy @NAME: asks NAME to let us see their screen(s).
        void ask(const std::string& who) {
            if (who.empty()) {
                start();
                return;
            }
            if (lowercase(who) == lowercase(chat_.name())) {
                chat_.notice("You do not need to spy on yourself.");
                return;
            }
            const std::string word = lowercase(who.substr(0, who.find(' ')));
            if (word == "allow" || word == "deny" || word == "dodge" || word == "yes" || word == "no") {
                chat_.notice("No peek is waiting for your answer (/spy @NAME asks someone to let you see their "
                             "screen).");
                return;
            }
            std::vector<std::pair<std::uint64_t, std::string>> found;
            for (const auto& [id, name] : chat_.people()) {
                if (lowercase(name) == lowercase(who)) {
                    found.emplace_back(id, name);
                }
            }
            if (found.empty()) {
                chat_.notice(std::format("Nobody called {} is in the chat (/who lists them).", text::sanitize(who, 48)));
                return;
            }
            if (found.size() > 1) {
                chat_.notice(std::format("{} people are called {}: one of them has to change name first.", found.size(),
                                         found.front().second));
                return;
            }
            const auto [target, target_name] = found.front();
            if (std::ranges::any_of(requests_, [&](const Request& r) {
                    return r.ours && r.other == target;
                })) {
                chat_.notice(std::format("You are already waiting for {} to answer a peek.",
                                         chat_.colored_name(target, target_name)));
                return;
            }
            const long long price = coins::price("spy").value_or(0);
            // Nobody pays unless the peek is accepted, but there is no point asking if we could not pay then.
            if (can_afford_ && !can_afford_(price)) {
                chat_.notice(std::format("A spy costs {} coins if they accept, and you do not have that many: win "
                                         "rounds of /game to earn more (/coins: yours).",
                                         price));
                return;
            }
            Request r;
            do {
                r.round = rng_();
            } while (r.round == 0 || known(r.round));
            r.ours = true;
            r.other = target;
            r.other_name = chat_.colored_name(target, target_name);
            r.other_plain = target_name;
            r.price = price;
            r.deadline = clock::now() + answer_time + patience;
            send(std::format("ask {:x} {:x} {} {} {}", r.round, target, price,
                             std::chrono::duration_cast<std::chrono::seconds>(answer_time).count(), chat_.name()));
            chat_.notice(std::format("👁 You ask {} to let you see their screen(s). They have {} seconds to /spy allow "
                                     "or /spy deny. You pay {} coins only if they accept.",
                                     r.other_name,
                                     std::chrono::duration_cast<std::chrono::seconds>(answer_time).count(), price));
            requests_.push_back(std::move(r));
        }

        // /spy allow|deny [NAME]: our answer to a peek someone asked of us.
        void answer(bool allow, const std::string& who) {
            std::vector<Request*> open;
            for (Request& r : requests_) {
                if (r.us && !r.answered && (who.empty() || lowercase(r.other_plain) == lowercase(who))) {
                    open.push_back(&r);
                }
            }
            if (open.empty()) {
                chat_.notice("No peek is waiting for your answer (/spy lists them).");
                return;
            }
            if (open.size() > 1 && who.empty()) {
                chat_.notice("More than one peek is waiting: say whose, like /spy allow NAME (/spy lists them).");
                return;
            }
            Request& r = *open.back();
            r.answered = true;
            const std::uint64_t round = r.round;
            const std::string other_name = r.other_name;
            const long long bounty = half_of(r.price);
            done_.push_back(round);
            // The request is over for us either way; the asker hears the outcome below (or the worker sends it).
            requests_.erase(std::ranges::find(requests_, round, &Request::round));
            if (!allow) {
                send_done(round, "denied", 0);
                chat_.notice(std::format("🕶 You refused {}'s peek: no screenshot was taken.", other_name));
                return;
            }
            chat_.notice(std::format("📸 You let {} see your screen(s): taking the picture now...", other_name));
            // Capturing runs outside tools and can take a moment: do it on a thread of its own, never the chat's.
            start_capture(round, other_name, bounty);
        }

        // Takes the screens and shares them, then tells everyone the outcome. Runs on its own thread.
        void start_capture(std::uint64_t round, std::string asker_name, long long bounty) {
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::jthread worker([this, round, asker_name = std::move(asker_name), bounty,
                                 done](std::stop_token stop) {
                // However this ends, mark it finished so tick() can let the thread go.
                struct Finish {
                    std::shared_ptr<std::atomic<bool>> flag;
                    ~Finish() {
                        flag->store(true);
                    }
                } finish {done};
                auto screens = capture::capture_screens(stop);
                if (!screens || screens->empty()) {
                    chat_.notice(std::format("Could not take the screenshot: {}.",
                                             screens ? "no screen" : screens.error()));
                    chat_.send_game(std::format("{} done {:x} {:x} failed 0 {}", game_name, round, chat_.id(),
                                                chat_.name()));
                    return;
                }
                std::size_t sent = 0;
                for (const auto& file : *screens) {
                    bool still = false;
                    auto picture = image::encode_picture(file, screen_pixels, &still);
                    if (picture) {
                        if (sent == 0) {
                            chat_.say(std::format("👁 Spied by {} — my {}:", strip_colors(asker_name),
                                                  screens_of(screens->size())));
                        }
                        chat_.send_picture(*picture);
                        ++sent;
                    }
                    std::error_code ec;
                    std::filesystem::remove(file, ec);
                }
                std::error_code ec;
                if (!screens->empty()) {
                    std::filesystem::remove(screens->front().parent_path(), ec);
                }
                if (sent == 0) {
                    chat_.send_game(std::format("{} done {:x} {:x} failed 0 {}", game_name, round, chat_.id(),
                                                chat_.name()));
                    chat_.notice("Could not share the screenshot.");
                    return;
                }
                // A thank-you for playing along: half of what the asker paid.
                if (cash_) {
                    cash_(bounty);
                }
                chat_.notice(std::format("📸 Shared {} with the chat; +{} coins for being a good sport.",
                                         screens_of(sent), bounty));
                chat_.send_game(
                    std::format("{} done {:x} {:x} shot {} {}", game_name, round, chat_.id(), sent, chat_.name()));
            });
            workers_.push_back({std::move(worker), std::move(done)});
        }

        void receive_ask(std::uint64_t sender, std::string_view name, std::uint64_t round, std::string_view text) {
            const auto target = parse_number(next_field(text), 16);
            const auto price = parse_number(next_field(text), 10);
            const auto seconds = parse_number(next_field(text), 10);
            if (!target || !price || !seconds || text.empty() || known(round)) {
                return;
            }
            if (*target != chat_.id()) {
                // Someone else is being asked: a quiet line, so the chat can see the peek going on.
                chat_.notice(std::format("👁 {} asks {} to show their screen(s).", chat_.colored_name(sender, name),
                                         chat_.colored_name(*target, text)));
                Request r;
                r.round = round;
                r.other = sender;
                r.other_name = chat_.colored_name(sender, name);
                r.other_plain = std::string(name);
                r.deadline = clock::now() + std::chrono::seconds(std::min<std::uint64_t>(*seconds, 600)) + patience;
                requests_.push_back(std::move(r));
                return;
            }
            Request r;
            r.round = round;
            r.us = true;
            r.other = sender;
            r.other_name = chat_.colored_name(sender, name);
            r.other_plain = std::string(name);
            r.price = static_cast<long long>(*price);
            r.deadline = clock::now() + std::chrono::seconds(std::min<std::uint64_t>(*seconds, 600));
            chat_.notice(std::format("👁 {} wants to see your screen(s)! Nothing is taken unless you say so: /spy allow "
                                     "shares them, /spy deny refuses ({} seconds, and doing nothing refuses).",
                                     r.other_name, *seconds));
            terminal_.bell();
            requests_.push_back(std::move(r));
        }

        void receive_done(std::uint64_t round, std::string_view text) {
            const auto target = parse_number(next_field(text), 16);
            const std::string_view result = next_field(text);
            const auto count = parse_number(next_field(text), 10);
            if (!target || !count || text.empty() ||
                (result != "shot" && result != "denied" && result != "failed" && result != "gone")) {
                return;
            }
            const auto it = std::ranges::find(requests_, round, &Request::round);
            const bool ours = it != requests_.end() && it->ours;
            // Only now that they accepted do we pay (and only us, the asker). A refusal costs nobody anything.
            const bool charge = ours && result == "shot";
            if (it != requests_.end()) {
                requests_.erase(it);
            } else if (known(round)) {
                return;
            }
            done_.push_back(round);
            if (charge && spend_) {
                spend_("spy"); // deducts the spy's price, and says so
            }
            const std::string other = chat_.colored_name(*target, text);
            if (result == "shot") {
                if (ours) {
                    chat_.notice(std::format("📸 {} let you in: {} shared above.", other, screens_of(*count)));
                } else {
                    chat_.notice(std::format("📸 {} shared {} for a peek.", other, screens_of(*count)));
                }
            } else if (result == "denied") {
                chat_.notice(std::format("🕶 {} refused the peek: no screenshot was taken{}.", other,
                                         ours ? ", and it cost you nothing" : ""));
            } else if (result == "failed") {
                chat_.notice(std::format("🖥 {} agreed, but their machine could not take a screenshot{}.", other,
                                         ours ? " — so you paid nothing" : ""));
            }
        }

        bool known(std::uint64_t round) const {
            return std::ranges::find(done_, round) != done_.end() ||
                   std::ranges::find(requests_, round, &Request::round) != requests_.end();
        }

        static std::string strip_colors(std::string_view s) {
            std::string out;
            for (std::size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\x1b') {
                    while (i < s.size() && s[i] != 'm') {
                        ++i;
                    }
                } else {
                    out += s[i];
                }
            }
            return out;
        }

        void send_done(std::uint64_t round, std::string_view result, std::size_t count) {
            const std::string text =
                std::format("done {:x} {:x} {} {} {}", round, chat_.id(), result, count, chat_.name());
            send(text);
            dones_.push_back({std::format("{} {}", game_name, text), done_sends - 1, clock::now() + resend_every});
        }

        void send(std::string_view event) {
            chat_.send_game(std::format("{} {}", game_name, event));
        }

        Chat& chat_;
        Screen& terminal_;
        std::function<bool(long long)> can_afford_;
        std::function<bool(std::string_view)> spend_;
        std::function<void(long long)> cash_;
        std::mt19937_64 rng_;
        std::vector<Request> requests_;
        // The requests that ended, so their late packets are ignored.
        std::deque<std::uint64_t> done_;
        // Our outcomes, sent again so the asker gets them.
        std::vector<Resend> dones_;
        // The threads taking screenshots; joined when they finish (and on destruction). The flag lets tick() know a
        // worker is done without joining (which would block).
        struct Worker {
            std::jthread thread;
            std::shared_ptr<std::atomic<bool>> done;
        };
        std::vector<Worker> workers_;
    };

} // namespace

std::unique_ptr<Game> make_spy(Chat& chat, Screen& terminal, std::function<bool(long long)> can_afford,
                               std::function<bool(std::string_view)> spend, std::function<void(long long)> cash) {
    return std::make_unique<Spy>(chat, terminal, std::move(can_afford), std::move(spend), std::move(cash));
}

} // namespace zchat::game
