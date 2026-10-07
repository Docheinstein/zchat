// Ratings: the Elo ratings of the games played in the chat (see game::Ratings and elo.hpp), one for each game and a
// general one that every game counts towards, and their leaderboards.
//
// There is no server to keep them, so each zchat is the one that keeps its user's own ratings, in the config ("elo"),
// and works out how they change from the results of the rounds it sees, with the ratings of the others as it last
// heard them. It tells the others what they are, as Game packets:
//   elo ratings <user> <ratings>     our ratings (see elo::encode()), with our user id (see Chat::user()) in hex
// when they change, every minute, and soon after it hears from somebody for the first time (who just came, most
// likely). The others keep what they hear in the "ratings" file of the config folder, by user id, which stays the
// same through changes of name and color: the leaderboards show who left too. They work out the new ratings of a
// round as well, to show them right away, but what the player's own zchat says is what counts.
// People whose zchat does not know about ratings are rated too, by name, for this session only.
//
// The coins won in the rounds (see coins.hpp) are kept the same way: ours in the config ("wallet"), the others' in
// the "coins" file of the config folder, as they tell them, both sealed (see coins::seal()) so they are not numbers
// to edit by hand:
//   elo coins <user> <coins>         our coins (see coins::encode()), with our user id
// along with our ratings. They are spent on the annoying commands, like /trill and /kick (see spend()).

#include "coins.hpp"
#include "config.hpp"
#include "elo.hpp"
#include "game.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <optional>
#include <random>
#include <set>
#include <system_error>

namespace zchat::game {

namespace {

    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;

    constexpr std::string_view game_name = "elo";
    constexpr auto heartbeat = 60s;
    // After starting: the chat must have joined first.
    constexpr auto first_announce = 2s;
    constexpr std::size_t board_size = 10;
    // Each of the leaderboards of /game leaderboard all.
    constexpr std::size_t short_board_size = 5;

    std::string lowercase(std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        return out;
    }

    std::optional<std::uint64_t> parse_hex(std::string_view s) {
        std::uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, 16);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size() || value == 0) {
            return std::nullopt;
        }
        return value;
    }

    // Takes the next field of s, up to sep, and removes it with sep.
    std::string_view next_field(std::string_view& s, char sep = ' ') {
        const auto at = s.find(sep);
        const std::string_view field = s.substr(0, at);
        s.remove_prefix(at == std::string_view::npos ? s.size() : at + 1);
        return field;
    }

    std::string plural(long long n, std::string_view what) {
        return std::format("{} {}{}", n, what, n == 1 ? "" : "s");
    }

    // A change of rating, as shown: "+12", "-8", "±0".
    std::string signed_change(long long change) {
        return change > 0 ? std::format("+{}", change) : change < 0 ? std::format("{}", change) : "±0";
    }

    long long shown(double rating) {
        return std::llround(rating);
    }

    class RatingsGame final : public Ratings {
    public:
        RatingsGame(Chat& chat, Screen& terminal, std::vector<std::string> rated_games) :
            chat_(chat),
            terminal_(terminal),
            rated_(std::move(rated_games)),
            rng_(std::random_device {}()) {
            announce_at_ = clock::now() + first_announce;
            load();
        }

        std::string_view name() const override {
            return game_name;
        }

        std::string_view summary() const override {
            return "the Elo ratings of the games";
        }

        bool listed() const override {
            return false;
        }

        bool rated() const override {
            return false;
        }

        void start() override {
        }

        void message(std::uint64_t, std::string_view, std::string_view) override {
        }

        void receive(std::uint64_t sender, std::string_view name, std::string_view text) override {
            const std::string_view kind = next_field(text);
            if (kind == "coins") {
                receive_coins(sender, name, text);
                return;
            }
            if (kind != "ratings") {
                return;
            }
            const auto user = parse_hex(next_field(text));
            auto ratings = elo::decode(text);
            // Another zchat with our config is us: our ratings are the ones we keep.
            if (!user || !ratings || *user == chat_.user() || name.empty()) {
                return;
            }
            Person& person = people_[*user];
            const bool changed =
                person.name != name || person.sender != sender || elo::encode(person.ratings) != elo::encode(*ratings);
            person.name = std::string(name);
            person.sender = sender;
            person.ratings = std::move(*ratings);
            by_sender_[sender] = *user;
            guests_.erase(lowercase(name));
            if (changed) {
                save_others();
            }
            // Somebody new, likely just come: they hear ours soon (a little later each, so that all who answer do
            // not answer at once).
            if (heard_.insert(sender).second && !reply_at_) {
                reply_at_ = clock::now() + std::chrono::milliseconds(std::uniform_int_distribution(300, 1500)(rng_));
            }
        }

        void tick() override {
            const auto now = clock::now();
            if (now >= announce_at_ || (reply_at_ && now >= *reply_at_)) {
                announce();
            }
            // The window may not be ready to show them yet.
            if (!coins_shown_) {
                show_coins();
            }
        }

        void rate(std::string_view game, const std::vector<Placing>& placings) override {
            // Each player once, the best placed first.
            std::vector<Placing> players;
            for (const Placing& p : placings) {
                if (std::ranges::find(players, p.id, &Placing::id) == players.end()) {
                    players.push_back(p);
                }
            }
            if (players.size() < 2) {
                return;
            }
            std::ranges::stable_sort(players, {}, &Placing::place);
            std::vector<elo::Ratings*> who;
            std::vector<int> places;
            bool us = false;
            for (const Placing& p : players) {
                who.push_back(&ratings_of(p, us));
                places.push_back(p.place);
            }
            std::string lines[2];
            const std::string_view kinds[2] = {game, elo::general};
            for (int k = 0; k < 2; ++k) {
                std::vector<elo::Rating> before;
                for (const elo::Ratings* r : who) {
                    const auto it = r->find(kinds[k]);
                    before.push_back(it == r->end() ? elo::Rating {} : it->second);
                }
                const auto deltas = elo::deltas(before, places);
                for (std::size_t i = 0; i < who.size(); ++i) {
                    elo::Rating& r = (*who[i])[std::string(kinds[k])];
                    r.rating = std::max(0.0, before[i].rating + deltas[i]);
                    r.games = before[i].games + 1;
                    lines[k] += std::format("{}{} {} ({})", i == 0 ? "" : ", ",
                                            chat_.colored_name(players[i].id, players[i].name), shown(r.rating),
                                            signed_change(shown(r.rating) - shown(before[i].rating)));
                }
            }
            chat_.game_notice(std::format("📈 Elo in {}: {}. In all games: {}.", game, lines[0], lines[1]));
            pay(players, places);
            if (us) {
                config::set("elo", elo::encode(own_));
                save_coins();
                announce();
                show_coins();
            }
            save_others();
        }

        void wallet(std::string_view name) override {
            name = trimmed(name);
            const auto found = find_person(name);
            if (!found) {
                chat_.game_notice(std::format("Nobody called {} has coins: /coins top lists who has.", name));
            } else if (found->us) {
                chat_.game_notice(std::format("💰 You have {}. /shop: what they buy; win rounds of /game to earn more.",
                                              plural(coins_, "coin")));
            } else if (found->coins) {
                chat_.game_notice(std::format("💰 {} has {}.", found->name, plural(*found->coins, "coin")));
            } else {
                chat_.game_notice(std::format("{} has no coins: their zchat is from before coins (/update gets the "
                                              "latest).",
                                              found->name));
            }
        }

        void richest() override {
            struct Rich {
                std::string name; // colored
                long long coins = 0;
                bool us = false;
            };
            std::vector<Rich> rows {{chat_.colored_own_name(), coins_, true}};
            for (const auto& [user, p] : people_) {
                if (p.coins) {
                    rows.push_back({chat_.colored_name(p.sender, p.name), *p.coins, false});
                }
            }
            std::ranges::stable_sort(rows, std::ranges::greater {}, &Rich::coins);
            chat_.game_notice("💰 Who has the most coins:");
            const auto line = [&](std::size_t i) {
                chat_.game_notice(
                    std::format("  {:>2}. {:>6} {}{}", i + 1, rows[i].coins, rows[i].name, rows[i].us ? " (you)" : ""));
            };
            for (std::size_t i = 0; i < rows.size() && i < board_size; ++i) {
                line(i);
            }
            // We are further down: where.
            if (const auto us = std::ranges::find(rows, true, &Rich::us);
                us - rows.begin() >= static_cast<std::ptrdiff_t>(board_size)) {
                if (us - rows.begin() > static_cast<std::ptrdiff_t>(board_size)) {
                    chat_.game_notice("  ...");
                }
                line(static_cast<std::size_t>(us - rows.begin()));
            }
            if (rows.size() > board_size) {
                chat_.game_notice(std::format("  ({} with coins in all)", rows.size()));
            }
        }

        void shop() override {
            chat_.game_notice(std::format("🛒 What coins buy (you have {}):", plural(coins_, "coin")));
            for (const coins::Item& item : coins::items) {
                chat_.game_notice(std::format("  {:>3}  {}", item.price, item.what));
            }
            chat_.game_notice(
                std::format("  Win rounds of /game to earn more: {} for playing, and {} more for each player "
                            "you beat ({} for a tie).",
                            plural(coins::for_playing, "coin"), coins::per_beaten, coins::per_tie));
        }

        bool spend(std::string_view item) override {
            const auto it = std::ranges::find(coins::items, item, &coins::Item::name);
            if (it == coins::items.end() || it->soon) {
                return false;
            }
            if (coins_ < it->price) {
                chat_.notice(std::format("💸 A {} costs {}, and you have {}: win rounds of /game to earn more "
                                         "(/shop lists the prices).",
                                         it->name, plural(it->price, "coin"), coins_));
                return false;
            }
            coins_ -= it->price;
            save_coins();
            announce();
            show_coins();
            chat_.notice(std::format("💸 {} for the {}: {} left.", plural(it->price, "coin"), it->name,
                                     plural(coins_, "coin")));
            return true;
        }

        bool stake(long long amount, std::string_view what) override {
            if (coins_ < amount) {
                chat_.game_notice(std::format("💸 You have {}, not enough to bet {} at {}: win rounds of /game to earn "
                                              "more.",
                                              plural(coins_, "coin"), amount, what));
                return false;
            }
            coins_ -= amount;
            changed_coins();
            return true;
        }

        bool can_afford(long long amount) const override {
            return coins_ >= amount;
        }

        void cash(long long amount) override {
            if (amount <= 0) {
                return;
            }
            coins_ = std::min(coins::most, coins_ + amount);
            changed_coins();
        }

        void leaderboard(std::string_view game) override {
            if (game.empty() || game == elo::general) {
                show_board(elo::general, board_size);
            } else if (game == "all") {
                bool any = false;
                for (const std::string_view kind : kinds()) {
                    if (!board(kind).empty()) {
                        show_board(kind, short_board_size);
                        any = true;
                    }
                }
                if (!any) {
                    chat_.game_notice("Nobody has an Elo rating yet: play a game (see /game), and win!");
                }
            } else if (std::ranges::find(rated_, game) != rated_.end()) {
                show_board(game, board_size);
            } else {
                chat_.game_notice(
                    std::format("No Elo leaderboard for {}: there is one for {}, all of them, and all games "
                                "together (/game leaderboard).",
                                game, list_games()));
            }
        }

        void profile(std::string_view name) override {
            name = trimmed(name);
            const auto found = find_person(name);
            if (!found) {
                chat_.game_notice(
                    std::format("Nobody called {} has an Elo rating: /game leaderboard lists who has one.", name));
                return;
            }
            const std::string& shown_name = found->name;
            const elo::Ratings* ratings = found->ratings;
            std::vector<std::string> lines;
            for (const std::string_view kind : kinds()) {
                const auto it = ratings->find(kind);
                if (it == ratings->end() || it->second.games == 0) {
                    continue;
                }
                const auto rows = board(kind);
                const auto at = std::ranges::find(rows, ratings, &Row::ratings);
                lines.push_back(std::format("  {:<10} {:>4}{}  #{} of {} · {}",
                                            kind == elo::general ? "all games" : kind, shown(it->second.rating),
                                            mark(it->second), at - rows.begin() + 1, rows.size(),
                                            plural(it->second.games, "game")));
            }
            if (lines.empty()) {
                chat_.game_notice(std::format("{} has not played a rated game yet.", shown_name));
                return;
            }
            chat_.game_notice(std::format("📊 Elo ratings of {}:", shown_name));
            for (const std::string& line : lines) {
                chat_.game_notice(line);
            }
            if (std::ranges::any_of(*ratings, [](const auto& r) {
                    return r.second.games > 0 && r.second.games < elo::provisional_games;
                })) {
                chat_.game_notice(std::format("  ? fewer than {} games: still settling", elo::provisional_games));
            }
        }

    private:
        // Somebody else, as last heard.
        struct Person {
            std::string name;
            std::uint64_t sender = 0;
            elo::Ratings ratings;
            // None when their zchat does not tell.
            std::optional<long long> coins = std::nullopt;
        };

        // Somebody found by name: their name, colored, their ratings and coins, and whether it is us.
        struct Found {
            std::string name;
            const elo::Ratings* ratings = nullptr;
            std::optional<long long> coins;
            bool us = false;
        };

        // A name as typed: the window's '@' list types it after an '@', which is not part of it.
        static std::string_view trimmed(std::string_view name) {
            while (!name.empty() && (name.front() == ' ' || name.front() == '@')) {
                name.remove_prefix(1);
            }
            while (!name.empty() && name.back() == ' ') {
                name.remove_suffix(1);
            }
            return name;
        }

        // The coins of a round's players (the best placed first, at places): ours are ours to change; the others'
        // change too, to show them, until they tell theirs.
        void pay(const std::vector<Placing>& players, const std::vector<int>& places) {
            const auto payouts = coins::payouts(places);
            std::string line;
            for (std::size_t i = 0; i < players.size(); ++i) {
                std::optional<long long> balance;
                if (players[i].id == chat_.id()) {
                    coins_ = std::min(coins::most, coins_ + payouts[i]);
                    balance = coins_;
                } else if (const auto it = by_sender_.find(players[i].id); it != by_sender_.end()) {
                    auto& theirs = people_[it->second].coins;
                    if (theirs) {
                        *theirs = std::min(coins::most, *theirs + payouts[i]);
                    }
                    balance = theirs;
                }
                line += std::format("{}{} +{}{}", i == 0 ? "" : ", ",
                                    chat_.colored_name(players[i].id, players[i].name), payouts[i],
                                    balance ? std::format(" ({})", *balance) : "");
            }
            chat_.game_notice(std::format("💰 Coins: {}.", line));
        }

        // Somebody's coins, as they tell them.
        void receive_coins(std::uint64_t sender, std::string_view name, std::string_view text) {
            const auto user = parse_hex(next_field(text));
            const auto amount = coins::decode(text);
            // Another zchat with our config is us: our coins are the ones we keep.
            if (!user || !amount || *user == chat_.user() || name.empty()) {
                return;
            }
            Person& person = people_[*user];
            const bool changed = person.name != name || person.sender != sender || person.coins != amount;
            person.name = std::string(name);
            person.sender = sender;
            person.coins = amount;
            by_sender_[sender] = *user;
            guests_.erase(lowercase(name));
            if (changed) {
                save_others();
            }
        }

        // Ours changed: kept, told, and shown.
        void changed_coins() {
            save_coins();
            announce();
            show_coins();
        }

        // Ours in the config, sealed. Versions before kept them as a plain number, in "coins".
        void save_coins() {
            config::set("wallet", coins::seal(coins_, chat_.user()));
            config::remove("coins");
        }

        // Our coins in the window, by our name.
        void show_coins() {
            coins_shown_ = terminal_.show_game("coins", std::format("{{\"coins\":{}}}", coins_));
        }

        // A line of a leaderboard; ratings is whose they are, to find somebody in it.
        struct Row {
            std::string name; // colored
            const elo::Ratings* ratings = nullptr;
            elo::Rating rating;
            bool us = false;
        };

        // The general rating, then the games'.
        std::vector<std::string_view> kinds() const {
            std::vector<std::string_view> out {elo::general};
            out.insert(out.end(), rated_.begin(), rated_.end());
            return out;
        }

        std::string list_games() const {
            std::string out;
            for (std::size_t i = 0; i < rated_.size(); ++i) {
                out += std::format("{}{}", i == 0 ? "" : i + 1 == rated_.size() ? " and " : ", ", rated_[i]);
            }
            return out;
        }

        // A provisional rating is marked.
        static std::string_view mark(const elo::Rating& r) {
            return r.games < elo::provisional_games ? "?" : " ";
        }

        // The ratings of a player of a round: ours, of somebody we heard from, or else of somebody by name.
        elo::Ratings& ratings_of(const Placing& p, bool& us) {
            if (p.id == chat_.id()) {
                us = true;
                return own_;
            }
            if (const auto it = by_sender_.find(p.id); it != by_sender_.end()) {
                return people_[it->second].ratings;
            }
            // A new sender id (a new color), not told about yet: by name.
            const std::string name = lowercase(p.name);
            for (auto& [user, person] : people_) {
                if (lowercase(person.name) == name) {
                    return person.ratings;
                }
            }
            Person& guest = guests_[name];
            guest.name = p.name;
            guest.sender = p.id;
            return guest.ratings;
        }

        // Who has a rating of kind, the best first.
        std::vector<Row> board(std::string_view kind) const {
            std::vector<Row> rows;
            const auto add = [&](const elo::Ratings& ratings, std::string name, bool us) {
                if (const auto it = ratings.find(kind); it != ratings.end() && it->second.games > 0) {
                    rows.push_back({std::move(name), &ratings, it->second, us});
                }
            };
            add(own_, chat_.colored_own_name(), true);
            for (const auto& [user, p] : people_) {
                add(p.ratings, chat_.colored_name(p.sender, p.name), false);
            }
            for (const auto& [key, p] : guests_) {
                add(p.ratings, chat_.colored_name(p.sender, p.name), false);
            }
            std::ranges::stable_sort(rows, [](const Row& a, const Row& b) {
                return a.rating.rating != b.rating.rating ? a.rating.rating > b.rating.rating
                                                          : a.rating.games > b.rating.games;
            });
            return rows;
        }

        void show_board(std::string_view kind, std::size_t size) {
            const auto rows = board(kind);
            const std::string what = kind == elo::general ? std::string("all games") : std::string(kind);
            if (rows.empty()) {
                chat_.game_notice(std::format("Nobody has an Elo rating in {} yet.", what));
                return;
            }
            chat_.game_notice(std::format("🏆 Elo leaderboard, {}:", what));
            bool provisional = false;
            const auto line = [&](std::size_t i) {
                const Row& row = rows[i];
                provisional = provisional || row.rating.games < elo::provisional_games;
                chat_.game_notice(std::format("  {:>2}. {:>4}{} {}{} · {}", i + 1, shown(row.rating.rating),
                                              mark(row.rating), row.name, row.us ? " (you)" : "",
                                              plural(row.rating.games, "game")));
            };
            for (std::size_t i = 0; i < rows.size() && i < size; ++i) {
                line(i);
            }
            // We are further down: where.
            if (const auto us = std::ranges::find(rows, true, &Row::us);
                us != rows.end() && us - rows.begin() >= static_cast<std::ptrdiff_t>(size)) {
                if (us - rows.begin() > static_cast<std::ptrdiff_t>(size)) {
                    chat_.game_notice("  ...");
                }
                line(static_cast<std::size_t>(us - rows.begin()));
            }
            if (rows.size() > size) {
                chat_.game_notice(std::format("  ({} rated in all)", rows.size()));
            }
            if (provisional) {
                chat_.game_notice(std::format("  ? fewer than {} games: still settling", elo::provisional_games));
            }
        }

        // Somebody by name, without minding case: us, whoever is called so, or else whose name starts so.
        std::optional<Found> find_person(std::string_view name) const {
            const Found us {chat_.colored_own_name(), &own_, coins_, true};
            if (name.empty() || lowercase(name) == lowercase(chat_.name())) {
                return us;
            }
            const std::string wanted = lowercase(name);
            const auto found = [&](const Person& p) {
                return Found {chat_.colored_name(p.sender, p.name), &p.ratings, p.coins, false};
            };
            std::optional<Found> prefix;
            const auto look = [&](const Person& p) {
                const std::string have = lowercase(p.name);
                if (have == wanted) {
                    return true;
                }
                if (!prefix && have.starts_with(wanted)) {
                    prefix = found(p);
                }
                return false;
            };
            for (const auto& [user, p] : people_) {
                if (look(p)) {
                    return found(p);
                }
            }
            for (const auto& [key, p] : guests_) {
                if (look(p)) {
                    return found(p);
                }
            }
            if (!prefix && lowercase(chat_.name()).starts_with(wanted)) {
                return us;
            }
            return prefix;
        }

        void announce() {
            announce_at_ = clock::now() + heartbeat;
            reply_at_.reset();
            if (chat_.user() != 0) {
                chat_.send_game(std::format("{} ratings {:x} {}", game_name, chat_.user(), elo::encode(own_)));
                chat_.send_game(std::format("{} coins {:x} {}", game_name, chat_.user(), coins::encode(coins_)));
            }
        }

        // A file of the others' (see save_others()), in the config folder.
        static std::filesystem::path others_file(std::string_view name) {
            const auto dir = config::dir();
            return dir.empty() ? dir : dir / name;
        }

        // Ours from the config, and the others' from their files: a line each, "USER\tSENDER\tNAME\tRATINGS", and
        // "USER\tSENDER\tNAME\tCOINS" for the coins, sealed.
        void load() {
            if (const auto saved = config::get("elo")) {
                own_ = elo::decode(*saved).value_or(elo::Ratings {});
            }
            if (const auto saved = config::get("wallet")) {
                if (const auto amount = coins::unseal(*saved, chat_.user())) {
                    coins_ = *amount;
                } else {
                    // Changed by hand, or copied from somebody else's config: back to the start.
                    coins_ = coins::initial;
                    save_coins();
                    chat_.notice(std::format("🚨 Your coins were tampered with: back to {}.",
                                             plural(coins_, "coin")));
                }
            } else if (const auto legacy = config::get("coins")) {
                coins_ = coins::decode(*legacy).value_or(coins::initial);
                save_coins();
            }
            const auto path = others_file("ratings");
            if (path.empty()) {
                return;
            }
            std::ifstream in(path);
            for (std::string line; std::getline(in, line);) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                std::string_view rest = line;
                const auto user = parse_hex(next_field(rest, '\t'));
                const auto sender = parse_hex(next_field(rest, '\t'));
                const std::string_view name = next_field(rest, '\t');
                auto ratings = elo::decode(rest);
                if (user && sender && !name.empty() && ratings && *user != chat_.user()) {
                    people_[*user] = {std::string(name), *sender, std::move(*ratings)};
                }
            }
            std::ifstream coins_in(others_file("coins"));
            for (std::string line; std::getline(coins_in, line);) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                std::string_view rest = line;
                const auto user = parse_hex(next_field(rest, '\t'));
                const auto sender = parse_hex(next_field(rest, '\t'));
                const std::string_view name = next_field(rest, '\t');
                std::optional<long long> amount;
                if (user) {
                    // Older versions kept them as plain numbers: only what they told, which they tell again.
                    amount = coins::unseal(rest, *user);
                    if (!amount) {
                        amount = coins::decode(rest);
                    }
                }
                if (user && sender && !name.empty() && amount && *user != chat_.user()) {
                    Person& p = people_[*user];
                    if (p.name.empty()) {
                        p.name = std::string(name);
                        p.sender = *sender;
                    }
                    p.coins = amount;
                }
            }
        }

        // The others' ratings, and their coins, each in a file of their own.
        void save_others() const {
            save_file("ratings", [](std::uint64_t, const Person& p) {
                return std::optional(elo::encode(p.ratings));
            });
            save_file("coins", [](std::uint64_t user, const Person& p) {
                return p.coins ? std::optional(coins::seal(*p.coins, user)) : std::nullopt;
            });
        }

        // A line for each of the others that what() has something for: "USER\tSENDER\tNAME\tWHAT".
        void save_file(std::string_view name,
                       const std::function<std::optional<std::string>(std::uint64_t, const Person&)>& what) const {
            const auto path = others_file(name);
            if (path.empty()) {
                return;
            }
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            // A temporary file moved over the old one, so a crash never leaves half of it.
            auto tmp = path;
            tmp += ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                for (const auto& [user, p] : people_) {
                    if (const auto text = what(user, p)) {
                        out << std::format("{:x}\t{:x}\t{}\t{}\n", user, p.sender, p.name, *text);
                    }
                }
                if (!out.flush()) {
                    return;
                }
            }
            std::filesystem::rename(tmp, path, ec);
        }

        Chat& chat_;
        Screen& terminal_;
        // The games with ratings, by name.
        std::vector<std::string> rated_;
        std::mt19937_64 rng_;
        elo::Ratings own_;
        long long coins_ = coins::initial;
        // Whether the window shows them as they are.
        bool coins_shown_ = false;
        // The others: by user id, those we heard from (ever); by lowercase name, those whose zchat does not tell.
        std::map<std::uint64_t, Person> people_;
        std::map<std::string, Person> guests_;
        // Who each sender id is, as heard.
        std::map<std::uint64_t, std::uint64_t> by_sender_;
        // The senders heard from in this session.
        std::set<std::uint64_t> heard_;
        clock::time_point announce_at_;
        std::optional<clock::time_point> reply_at_;
    };

} // namespace

std::unique_ptr<Ratings> make_ratings(Chat& chat, Screen& terminal, std::vector<std::string> rated_games) {
    return std::make_unique<RatingsGame>(chat, terminal, std::move(rated_games));
}

} // namespace zchat::game
