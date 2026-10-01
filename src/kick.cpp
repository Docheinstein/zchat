// Kick: /kick NAME asks the rest of the chat to vote someone out. Nobody is kicked right away: the others get
// vote_time to answer /kick yes (kick them) or /kick no (grace them), and when the time is up, or everybody voted,
// the one asked about is out if at least as many voted to kick as to grace (whoever asked counts as a vote to kick;
// whoever does not answer does not count). Kicked out, their zchat closes. Asking costs coins (see coins.hpp); voting
// is free.
//
// Not a game, but it travels the same way: whoever asks is the referee of the vote, and counts the ballots. As Game
// packets:
//   kick vote <round> <target> <seconds> <voters> <name>  who asks, about whom (<target>, and their <name>), for
//                                                         how long, and how many vote (them included)
//   kick ballot <round> yes|no                            a vote, sent to the referee (and again, until counted)
//   kick voted <round> <voter> <count> <voters> <name>    the referee counted a vote: <count> voted so far
//   kick result <round> <target> <kick> <grace> out|stay|gone <name>   the end: the votes to kick and to grace, and
//                                                         whether <name> is out, stays, or left before the end
// where <round> is a random hex number naming the vote, and <target> and <voter> sender ids, in hex. The result is
// sent a few times, as it matters most: the one kicked has to get it.

#include "game.hpp"

#include "color.hpp"
#include "text.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <deque>
#include <format>
#include <map>
#include <optional>
#include <random>
#include <vector>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;

    constexpr std::string_view game_name = "kick";
    constexpr auto vote_time = 20s;
    // How much longer the voters wait for the referee's result, before thinking it left.
    constexpr auto patience = 5s;
    // How often a vote not counted yet is sent again, and the result too, and how many times.
    constexpr auto resend_every = 2s;
    constexpr int result_sends = 3;

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

    std::string hex_color(std::uint64_t id) {
        const Color c = color_of_id(id);
        return std::format("#{:02x}{:02x}{:02x}", c.r, c.g, c.b);
    }

    std::string votes_of(std::size_t n) {
        return std::format("{} vote{}", n, n == 1 ? "" : "s");
    }

    class Kick final : public Game {
    public:
        Kick(Chat& chat, Screen& terminal, std::function<void()> kicked, std::function<bool(std::string_view)> spend) :
            chat_(chat),
            terminal_(terminal),
            kicked_(std::move(kicked)),
            spend_(std::move(spend)),
            rng_(std::random_device {}()) {
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "vote someone out of the chat";
        }

        bool listed() const override {
            return false;
        }

        bool keeps_case() const override {
            return true;
        }

        // /kick alone: how it works, and the votes on.
        void start() override {
            chat_.notice("/kick NAME asks the others to vote NAME out of the chat: /kick yes kicks them, /kick no graces "
                         "them.");
            chat_.notice(std::format("   In {} seconds they are out if at least as many vote to kick as to grace "
                                     "(asking counts as a vote to kick), and their zchat closes.",
                                     vote_time.count()));
            for (const Vote& v : votes_) {
                chat_.notice(std::format("   On now: {} out? {}", v.target_name,
                                         v.ours        ? std::string("you asked")
                                         : v.us        ? std::string("about you")
                                         : v.mine      ? std::format("you voted {}", *v.mine ? "yes" : "no")
                                                       : std::string("/kick yes or /kick no")));
            }
        }

        // /kick NAME, or /kick yes|no [NAME] while a vote is on.
        bool command(std::string_view args) override {
            std::string_view rest = args;
            const std::string choice = lowercase(next_field(rest));
            const bool yes = choice == "yes" || choice == "y";
            const bool no = choice == "no" || choice == "n" || choice == "grace";
            // Someone could be called "yes": it is a vote only when there is one to vote in.
            if ((yes || no) && std::ranges::any_of(votes_, [](const Vote& v) {
                    return !v.ours && !v.us;
                })) {
                vote(yes, person(rest));
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
            if (event == "vote") {
                receive_vote(sender, name, *round, text);
            } else if (event == "ballot") {
                receive_ballot(sender, name, *round, text);
            } else if (event == "voted") {
                receive_voted(*round, text);
            } else if (event == "result") {
                receive_result(*round, text);
            }
        }

        void message(std::uint64_t, std::string_view, std::string_view) override {}

        void tick() override {
            const auto now = clock::now();
            for (auto it = votes_.begin(); it != votes_.end();) {
                Vote& v = *it;
                if (v.ours) {
                    const auto people = chat_.people();
                    const bool here = std::ranges::any_of(people, [&](const auto& p) {
                        return p.first == v.target;
                    });
                    if (!here || now >= v.deadline || v.ballots.size() == v.voters.size()) {
                        Vote done = std::move(v);
                        it = votes_.erase(it);
                        finish(done, here);
                        continue;
                    }
                } else if (now >= v.deadline) {
                    chat_.notice(std::format("🗳 The vote on kicking {} ended without a result: {} may have left.",
                                             v.target_name, v.referee_name));
                    done_.push_back(v.round);
                    it = votes_.erase(it);
                    continue;
                } else if (v.mine && !v.counted && now - v.sent >= resend_every) {
                    send_ballot(v);
                }
                ++it;
            }
            for (auto it = results_.begin(); it != results_.end();) {
                if (now < it->next) {
                    ++it;
                    continue;
                }
                send(it->text);
                it->next = now + resend_every;
                if (--it->left == 0) {
                    it = results_.erase(it);
                } else {
                    ++it;
                }
            }
            while (done_.size() > 64) {
                done_.pop_front();
            }
            publish();
        }

    private:
        struct Vote {
            std::uint64_t round = 0;
            // We asked: we are its referee.
            bool ours = false;
            std::string referee_name; // colored
            std::uint64_t target = 0;
            std::string target_name; // colored
            std::string target_plain;
            // We are the one it is about.
            bool us = false;
            std::size_t voters_count = 0;
            std::size_t voted = 0;
            clock::time_point deadline;
            // For the referee: who votes (not us, nor the target), by id, and their votes (true to kick).
            std::map<std::uint64_t, std::string> voters;
            std::map<std::uint64_t, bool> ballots;
            // For the voters: our vote, when it was last sent, and whether the referee counted it.
            std::optional<bool> mine;
            clock::time_point sent;
            bool counted = false;
        };

        struct Resend {
            std::string text;
            int left = 0;
            clock::time_point next;
        };

        // /kick NAME: starts a vote on kicking NAME, with us as the referee.
        void ask(const std::string& who) {
            if (who.empty()) {
                start();
                return;
            }
            std::vector<std::pair<std::uint64_t, std::string>> found;
            const auto people = chat_.people();
            for (const auto& [id, name] : people) {
                if (lowercase(name) == lowercase(who)) {
                    found.emplace_back(id, name);
                }
            }
            if (found.empty()) {
                const std::string word = lowercase(who.substr(0, who.find(' ')));
                if (lowercase(who) == lowercase(chat_.name())) {
                    chat_.notice("You cannot kick yourself: /quit leaves.");
                } else if (word == "yes" || word == "y" || word == "no" || word == "n" || word == "grace") {
                    chat_.notice(std::ranges::any_of(votes_, &Vote::us)
                                     ? "The vote is about you: the others decide."
                                     : "No vote on kicking someone is on for you to vote in (/kick NAME starts one).");
                } else {
                    chat_.notice(std::format("Nobody called {} is in the chat (/who lists them).",
                                             text::sanitize(who, 48)));
                }
                return;
            }
            if (found.size() > 1) {
                chat_.notice(std::format("{} people are called {}: one of them has to change name first.",
                                         found.size(), found.front().second));
                return;
            }
            const auto [target, target_name] = found.front();
            if (std::ranges::any_of(votes_, [&](const Vote& v) {
                    return v.target == target;
                })) {
                chat_.notice(std::format("A vote on kicking {} is on already.", chat_.colored_name(target, target_name)));
                return;
            }
            Vote v;
            for (const auto& [id, name] : people) {
                if (id != target) {
                    v.voters.emplace(id, name);
                }
            }
            if (v.voters.empty()) {
                chat_.notice(std::format("Nobody else is in the chat to vote on kicking {}: it takes someone else "
                                         "to agree.",
                                         chat_.colored_name(target, target_name)));
                return;
            }
            // Asking costs coins (see coins.hpp), whatever the chat says.
            if (spend_ && !spend_("kick")) {
                return;
            }
            do {
                v.round = rng_();
            } while (v.round == 0);
            v.ours = true;
            v.referee_name = chat_.colored_own_name();
            v.target = target;
            v.target_name = chat_.colored_name(target, target_name);
            v.target_plain = target_name;
            v.voters_count = v.voters.size() + 1;
            v.voted = 1;
            v.deadline = clock::now() + vote_time;
            send(std::format("vote {:x} {:x} {} {} {}", v.round, target, vote_time.count(), v.voters_count,
                             target_name));
            chat_.notice(std::format("🗳 You ask the chat to kick {} out. The others have {} seconds to vote: {} is "
                                     "out if at least as many vote to kick as to grace (yours is to kick).",
                                     v.target_name, vote_time.count(), v.target_name));
            votes_.push_back(std::move(v));
        }

        // /kick yes|no [NAME]: our vote, in the vote on NAME (the only one on, without a name).
        void vote(bool kick, const std::string& who) {
            std::vector<Vote*> open;
            for (Vote& v : votes_) {
                if (!v.ours && !v.us && (who.empty() || lowercase(v.target_plain) == lowercase(who))) {
                    open.push_back(&v);
                }
            }
            if (open.empty()) {
                chat_.notice(std::format("No vote on kicking {} is on (/kick lists them).", text::sanitize(who, 48)));
                return;
            }
            // Named, it is the latest vote on them (an older one may be left over from someone who left).
            if (open.size() > 1 && who.empty()) {
                chat_.notice("Several votes are on: say which, like /kick yes NAME (/kick lists them).");
                return;
            }
            Vote& v = *open.back();
            const bool changed = v.mine.has_value();
            v.mine = kick;
            v.counted = false;
            send_ballot(v);
            chat_.notice(std::format("🗳 You {}vote to {} {}.", changed ? "now " : "", kick ? "kick" : "grace",
                                     v.target_name));
            publish();
        }

        void send_ballot(Vote& v) {
            v.sent = clock::now();
            send(std::format("ballot {:x} {}", v.round, *v.mine ? "yes" : "no"));
        }

        void receive_vote(std::uint64_t sender, std::string_view name, std::uint64_t round, std::string_view text) {
            const auto target = parse_number(next_field(text), 16);
            const auto seconds = parse_number(next_field(text), 10);
            const auto voters = parse_number(next_field(text), 10);
            if (!target || !seconds || !voters || text.empty() || known(round)) {
                return;
            }
            Vote v;
            v.round = round;
            v.referee_name = chat_.colored_name(sender, name);
            v.target = *target;
            v.target_name = chat_.colored_name(*target, text);
            v.target_plain = std::string(text);
            v.us = *target == chat_.id();
            v.voters_count = *voters;
            v.voted = 1;
            v.deadline = clock::now() + std::chrono::seconds(std::min<std::uint64_t>(*seconds, 600)) + patience;
            if (v.us) {
                chat_.notice(std::format("🗳 {} wants you kicked out of the chat! The others are voting: in {} "
                                         "seconds, you are out if at least as many vote to kick you as to grace you.",
                                         v.referee_name, *seconds));
            } else {
                chat_.notice(std::format("🗳 {} wants to kick {} out of the chat: /kick yes to kick them, /kick no to "
                                         "grace them ({} seconds).",
                                         v.referee_name, v.target_name, *seconds));
            }
            terminal_.bell();
            votes_.push_back(std::move(v));
        }

        // For the referee: a vote, counted (again: it may be changed) and acknowledged.
        void receive_ballot(std::uint64_t sender, std::string_view name, std::uint64_t round, std::string_view text) {
            const auto it = std::ranges::find(votes_, round, &Vote::round);
            if (it == votes_.end() || !it->ours || !it->voters.contains(sender) || (text != "yes" && text != "no")) {
                return;
            }
            it->ballots[sender] = text == "yes";
            send(std::format("voted {:x} {:x} {} {} {}", round, sender, it->ballots.size() + 1, it->voters_count,
                             name));
            announce_voted(*it, it->ballots.size() + 1, chat_.colored_name(sender, name));
        }

        void receive_voted(std::uint64_t round, std::string_view text) {
            const auto it = std::ranges::find(votes_, round, &Vote::round);
            const auto voter = parse_number(next_field(text), 16);
            const auto count = parse_number(next_field(text), 10);
            next_field(text);
            if (it == votes_.end() || it->ours || !voter || !count || text.empty()) {
                return;
            }
            if (*voter == chat_.id()) {
                it->counted = true;
            }
            announce_voted(*it, *count, chat_.colored_name(*voter, text));
        }

        // Who voted, shown once (how is secret, until the result).
        void announce_voted(Vote& v, std::size_t count, const std::string& voter) {
            if (count <= v.voted) {
                return;
            }
            v.voted = count;
            chat_.notice(std::format("🗳 {} voted on kicking {} ({} of {}).", voter, v.target_name, count,
                                     v.voters_count));
        }

        void receive_result(std::uint64_t round, std::string_view text) {
            const auto target = parse_number(next_field(text), 16);
            const auto kick = parse_number(next_field(text), 10);
            const auto grace = parse_number(next_field(text), 10);
            const std::string_view outcome = next_field(text);
            if (!target || !kick || !grace || text.empty() || (outcome != "out" && outcome != "stay" &&
                                                               outcome != "gone")) {
                return;
            }
            const auto it = std::ranges::find(votes_, round, &Vote::round);
            if (it != votes_.end() && it->ours) {
                return;
            }
            const bool us = *target == chat_.id() || (it != votes_.end() && it->us);
            if (it != votes_.end()) {
                votes_.erase(it);
            } else if (known(round)) {
                return;
            }
            done_.push_back(round);
            announce_result(chat_.colored_name(*target, text), us, *kick, *grace, outcome);
        }

        // For the referee: the votes are in, or the time is up, or the one it is about left.
        void finish(const Vote& v, bool here) {
            const std::size_t kick = 1 + static_cast<std::size_t>(std::ranges::count_if(v.ballots, [](const auto& b) {
                                             return b.second;
                                         }));
            const std::size_t grace = v.ballots.size() + 1 - kick;
            const std::string_view outcome = !here ? "gone" : kick >= grace ? "out" : "stay";
            const std::string result =
                std::format("result {:x} {:x} {} {} {} {}", v.round, v.target, kick, grace, outcome, v.target_plain);
            send(result);
            results_.push_back({result, result_sends - 1, clock::now() + resend_every});
            done_.push_back(v.round);
            announce_result(v.target_name, false, kick, grace, outcome);
        }

        void announce_result(const std::string& target, bool us, std::size_t kick, std::size_t grace,
                             std::string_view outcome) {
            const std::string tally = std::format("{} to kick, {} to grace", votes_of(kick), votes_of(grace));
            if (outcome == "gone") {
                chat_.notice(std::format("🗳 {} left before the vote on kicking them ended.", target));
            } else if (outcome == "stay") {
                chat_.notice(std::format("😇 {} {} graced and stay{} in the chat: {}.", us ? "You" : target,
                                         us ? "are" : "is", us ? "" : "s", tally));
            } else if (!us) {
                chat_.notice(std::format("👢 {} is kicked out of the chat: {}.", target, tally));
            } else if (!kicked_out_) {
                kicked_out_ = true;
                chat_.notice(std::format("👢 The chat voted you out: {}. Bye!", tally));
                terminal_.bell();
                if (kicked_) {
                    kicked_();
                }
            }
        }

        bool known(std::uint64_t round) const {
            return std::ranges::find(done_, round) != done_.end() ||
                   std::ranges::find(votes_, round, &Vote::round) != votes_.end();
        }

        // The votes we can vote in, for the window to show them with buttons to kick or grace (see the kick votes in
        // src/ui/index.html), each time they change:
        //   {"votes": [{"round": "ab12", "name": "Rex", "color": "#rrggbb", "by": "Ash", "byColor": "#rrggbb",
        //               "left": seconds}...]}
        void publish() {
            std::string list;
            std::string shown;
            const auto now = clock::now();
            for (const Vote& v : votes_) {
                if (v.ours || v.us || v.mine) {
                    continue;
                }
                const auto left = std::chrono::duration_cast<std::chrono::seconds>(v.deadline - patience - now);
                list += std::format("{}{{\"round\":\"{:x}\",\"name\":{},\"color\":\"{}\",\"by\":{},\"left\":{}}}",
                                    list.empty() ? "" : ",", v.round, json(v.target_plain), hex_color(v.target),
                                    json(strip_colors(v.referee_name)), std::max<long long>(0, left.count()));
                shown += std::format("{:x} ", v.round);
            }
            if (shown == shown_) {
                return;
            }
            // Until the window is there to show them (a terminal never is: there they are typed).
            if (terminal_.show_game("kick", std::format("{{\"votes\":[{}]}}", list))) {
                shown_ = shown;
            }
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

        void send(std::string_view event) {
            chat_.send_game(std::format("{} {}", game_name, event));
        }

        Chat& chat_;
        Screen& terminal_;
        std::function<void()> kicked_;
        std::function<bool(std::string_view)> spend_;
        std::mt19937_64 rng_;
        std::vector<Vote> votes_;
        // The votes that ended, so their late packets are ignored.
        std::deque<std::uint64_t> done_;
        // Our results, sent again.
        std::vector<Resend> results_;
        // The votes shown in the window, by round.
        std::string shown_;
        bool kicked_out_ = false;
    };

} // namespace

std::unique_ptr<Game> make_kick(Chat& chat, Screen& terminal, std::function<void()> kicked,
                                std::function<bool(std::string_view)> spend) {
    return std::make_unique<Kick>(chat, terminal, std::move(kicked), std::move(spend));
}

} // namespace zchat::game
