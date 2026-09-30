#include "chat.hpp"

#include "cipher.hpp"
#include "image.hpp"
#include "markup.hpp"
#include "text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <ctime>
#include <format>
#include <utility>

namespace zchat {

namespace {

    using namespace std::chrono_literals;

    constexpr auto heartbeat_interval = 5s;
    constexpr auto peer_timeout = 16s;
    constexpr auto interfaces_refresh_interval = 30s;
    constexpr std::size_t dedup_window = 64;
    // "@everyone" tags all the people in the chat.
    constexpr std::string_view everyone_tag = "everyone";

    std::optional<std::uint64_t> parse_id(std::string_view s) {
        std::uint64_t id = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), id, 16);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size() || id == 0) {
            return std::nullopt;
        }
        return id;
    }

    // Pictures in Chunks, see Chat::send_picture().
    constexpr std::size_t max_outgoing_pictures = 8;
    constexpr auto outgoing_picture_lifetime = 2min;
    constexpr std::size_t max_incoming_pictures = 8;
    constexpr std::size_t max_received_pictures = 64;
    // How long after the last piece the missing ones are asked for (twice as long after each request that brings
    // nothing, up to chunk_max_wait); when a picture is given up on; how soon a piece is not sent again, for
    // everybody who asked for it at about the same time.
    constexpr auto chunk_wait = 300ms;
    constexpr auto chunk_max_wait = 2400ms;
    constexpr auto chunk_give_up = 20s;
    constexpr auto chunk_resend_interval = 250ms;

    // "A B C": three numbers.
    bool parse_numbers(std::string_view s, std::uint64_t& a, std::size_t& b, std::size_t& c) {
        const char* p = s.data();
        const char* end = s.data() + s.size();
        std::uint64_t values[3] {};
        for (int i = 0; i < 3; ++i) {
            const auto [ptr, ec] = std::from_chars(p, end, values[i]);
            if (ec != std::errc {} || (i < 2 ? ptr == end || *ptr != ' ' : ptr != end)) {
                return false;
            }
            p = ptr + (i < 2 ? 1 : 0);
        }
        a = values[0];
        b = static_cast<std::size_t>(values[1]);
        c = static_cast<std::size_t>(values[2]);
        return true;
    }

    std::tm local_time(std::time_t t) {
        std::tm local {};
#ifdef _WIN32
        localtime_s(&local, &t);
#else
        localtime_r(&t, &local);
#endif
        return local;
    }

    // The time of day, now or at t; with the date too when that is not today.
    std::string timestamp(std::time_t t = std::time(nullptr)) {
        const std::tm local = local_time(t);
        const std::tm today = local_time(std::time(nullptr));
        if (local.tm_year != today.tm_year || local.tm_yday != today.tm_yday) {
            return std::format("{}-{:02}-{:02} {:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                               local.tm_hour, local.tm_min);
        }
        return std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
    }

    std::string lowercase(std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        return out;
    }

    bool is_word_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

} // namespace

Chat::Chat(std::uint16_t port, std::string name, std::optional<Color> color, Screen& terminal) :
    id_(make_id(color)),
    name_(std::move(name)),
    terminal_(terminal),
    socket_(port) {
}

Chat::~Chat() {
    stop();
}

void Chat::start() {
    thread_ = std::jthread([this](std::stop_token stop) {
        run(stop);
    });
    send(PacketType::Join);
}

void Chat::stop() {
    if (stopped_.exchange(true)) {
        return;
    }
    // The thread first: what it sends after our Leave (a heartbeat, a game's packet) would bring us back.
    thread_.request_stop();
    if (thread_.joinable()) {
        thread_.join();
    }
    send(PacketType::Leave);
}

void Chat::say(std::string_view text) {
    const std::string clean = text::sanitize(text, max_text_bytes);
    send(PacketType::Message, clean);
    print_message(id_, name(), clean);
    history::add({std::time(nullptr), id_, name(), clean});
    std::scoped_lock lock(hooks_mutex_);
    if (hooks_.message) {
        hooks_.message(id_, name(), clean);
    }
}

void Chat::set_game_hooks(GameHooks hooks) {
    std::scoped_lock lock(hooks_mutex_);
    hooks_ = std::move(hooks);
}

void Chat::send_game(std::string_view text) {
    send(PacketType::Game, text);
}

void Chat::draw(std::string_view art) {
    Packet packet;
    packet.type = PacketType::Art;
    packet.name = name();
    packet.text = std::string(art);
    // Cleaned up the same way the others will see it.
    const std::string clean = decode(encode(packet)).value_or(Packet {}).text;
    send(PacketType::Art, clean);
    print_art(id_, name(), clean);
}

void Chat::send_picture(std::string_view picture) {
    Packet packet;
    packet.type = PacketType::Image;
    packet.name = name();
    packet.text = std::string(picture);
    const std::string clean = decode(encode(packet)).value_or(Packet {}).text;
    print_picture(id_, name(), clean);
    if (clean.size() <= max_chunk_bytes) {
        send(PacketType::Image, clean);
        return;
    }
    // In pieces; the others ask again for the ones they miss, from the ones kept in outgoing_. They can be asked for
    // only once all were sent: until then, the others are just waiting for them.
    Outgoing out {id_, next_picture_++, {}, {}, clock::now()};
    for (std::size_t i = 0; i < clean.size(); i += max_chunk_bytes) {
        out.pieces.emplace_back(std::string_view(clean).substr(i, max_chunk_bytes));
    }
    out.resent.resize(out.pieces.size());
    for (std::size_t i = 0; i < out.pieces.size(); ++i) {
        send_chunk(out.picture, i, out.pieces.size(), out.pieces[i]);
    }
    std::scoped_lock lock(outgoing_mutex_);
    while (!outgoing_.empty() && (outgoing_.size() >= max_outgoing_pictures ||
                                  clock::now() - outgoing_.front().sent > outgoing_picture_lifetime)) {
        outgoing_.pop_front();
    }
    outgoing_.push_back(std::move(out));
}

void Chat::send_chunk(std::uint64_t picture, std::size_t index, std::size_t count, std::string_view piece) {
    std::string text = std::format("{} {} {}\n", picture, index, count);
    text += piece;
    send(PacketType::Chunk, text, true);
}

void Chat::receive_chunk(const Packet& packet) {
    const auto nl = packet.text.find('\n');
    if (nl == std::string::npos) {
        return;
    }
    std::uint64_t picture = 0;
    std::size_t index = 0;
    std::size_t count = 0;
    if (!parse_numbers(std::string_view(packet.text).substr(0, nl), picture, index, count) || count < 2 ||
        count > max_chunks || index >= count) {
        return;
    }
    const std::pair key {packet.sender, picture};
    if (std::ranges::find(received_pictures_, key) != received_pictures_.end()) {
        return;
    }
    auto it = incoming_.find(key);
    if (it == incoming_.end()) {
        if (incoming_.size() >= max_incoming_pictures) {
            return;
        }
        it = incoming_.emplace(key, Incoming {packet.name, std::vector<std::string>(count), std::vector<bool>(count),
                                              0, clock::now(), clock::now(), chunk_wait})
                 .first;
    }
    Incoming& in = it->second;
    if (in.pieces.size() != count || in.have[index]) {
        return;
    }
    in.pieces[index] = packet.text.substr(nl + 1);
    in.have[index] = true;
    in.last_piece = clock::now();
    in.wait = chunk_wait;
    if (++in.received < count) {
        return;
    }
    std::string text;
    for (const auto& piece : in.pieces) {
        text += piece;
    }
    const std::string name = std::move(in.name);
    incoming_.erase(it);
    received_pictures_.push_back(key);
    if (received_pictures_.size() > max_received_pictures) {
        received_pictures_.pop_front();
    }
    print_picture(packet.sender, name, text);
}

void Chat::request_missing_chunks() {
    const auto now = clock::now();
    for (auto it = incoming_.begin(); it != incoming_.end();) {
        Incoming& in = it->second;
        if (now - in.last_piece > chunk_give_up) {
            notice(std::format("A picture from {} did not arrive whole.", colored_name(it->first.first, in.name)));
            it = incoming_.erase(it);
            continue;
        }
        // Once the pieces stop coming: the ones missing, as many as fit in a request.
        if (now - in.last_piece >= in.wait && now - in.last_request >= in.wait) {
            std::string request = std::format("{:x} {}", it->first.first, it->first.second);
            for (std::size_t i = 0; i < in.have.size() && request.size() + 12 < max_resend_bytes; ++i) {
                if (!in.have[i]) {
                    request += std::format(" {}", i);
                }
            }
            send(PacketType::Resend, request);
            in.last_request = now;
            in.wait = std::min<clock::duration>(in.wait * 2, chunk_max_wait);
        }
        ++it;
    }
}

void Chat::resend_chunks(std::string_view request) {
    const auto space = request.find(' ');
    if (space == std::string_view::npos) {
        return;
    }
    const auto sender = parse_id(request.substr(0, space));
    request.remove_prefix(space + 1);
    std::vector<std::size_t> indices;
    std::uint64_t picture = 0;
    bool first = true;
    while (!request.empty()) {
        const auto end = std::min(request.find(' '), request.size());
        std::uint64_t n = 0;
        const auto [ptr, ec] = std::from_chars(request.data(), request.data() + end, n);
        if (ec != std::errc {} || ptr != request.data() + end) {
            return;
        }
        if (first) {
            picture = n;
            first = false;
        } else {
            indices.push_back(static_cast<std::size_t>(n));
        }
        request.remove_prefix(std::min(end + 1, request.size()));
    }
    std::scoped_lock lock(outgoing_mutex_);
    const auto it = std::ranges::find_if(outgoing_, [&](const Outgoing& out) {
        return sender && out.sender == *sender && out.picture == picture;
    });
    if (it == outgoing_.end()) {
        return;
    }
    const auto now = clock::now();
    for (const std::size_t i : indices) {
        if (i < it->pieces.size() && now - it->resent[i] >= chunk_resend_interval) {
            it->resent[i] = now;
            send_chunk(picture, i, it->pieces.size(), it->pieces[i]);
        }
    }
}

void Chat::set_name(std::string name) {
    {
        std::scoped_lock lock(name_mutex_);
        name_ = std::move(name);
    }
    // Peers notice the new name on any packet from us; a heartbeat now saves waiting for the next one.
    send(PacketType::Here);
}

void Chat::set_color(Color color) {
    const std::uint64_t old_id = id_;
    if (color_of_id(old_id) == color) {
        return;
    }
    const std::uint64_t new_id = make_id(color);
    {
        std::scoped_lock lock(peers_mutex_);
        old_ids_.push_back(old_id);
    }
    // The color comes from the id, so we become a new peer. The Leave of the old id names the new one: newer
    // zchat versions just move us over, older ones see us leave and be back right away.
    send(PacketType::Leave, std::format("{:x}", new_id));
    id_ = new_id;
    send(PacketType::Here);
}

std::string Chat::paint(Color color, std::string_view text) const {
    if (!terminal_.colors()) {
        return std::string(text);
    }
    return std::format("\x1b[{}m{}\x1b[0m", ansi_foreground(color), text);
}

void Chat::print_history(const std::vector<history::Entry>& entries) const {
    for (const auto& entry : entries) {
        // Tags are shown, but ring no bell: they were heard back then.
        print_message(entry.sender, entry.name, entry.text, entry.time);
    }
}

bool Chat::print_message(std::uint64_t id, std::string_view name, std::string_view text,
                         std::optional<std::time_t> when) const {
    const std::string stamp = when ? timestamp(*when) : timestamp();
    const std::string time = terminal_.colors() ? std::format("\x1b[90m{}\x1b[0m", stamp) : stamp;
    bool tags_us = false;
    const std::string marked = mark_mentions(text, tags_us);
    terminal_.print(
        std::format("{} {}: {}", time, colored_name(id, name), markup::render(marked, terminal_.colors())));
    return tags_us;
}

std::string Chat::mark_mentions(std::string_view text, bool& tags_us) const {
    struct Person {
        std::string name;
        Color color;
        bool us;
        bool everyone = false;
    };
    std::vector<Person> people;
    // "@everyone" tags us too. First, so it wins over someone named "everyone": that tags them anyway.
    people.push_back({std::string(everyone_tag), {}, true, true});
    const std::uint64_t own_id = id_;
    people.push_back({lowercase(name()), color_of_id(own_id), true});
    {
        std::scoped_lock lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            people.push_back({lowercase(peer.name), color_of_id(id), false});
        }
    }
    // A name with '<' or '>' would break the markup around it.
    std::erase_if(people, [](const Person& p) {
        return p.name.empty() || p.name.find_first_of("<>") != std::string::npos;
    });
    // The longest name first, so "@Rex Jr" is not taken for "@Rex".
    std::ranges::stable_sort(people, std::ranges::greater {}, [](const Person& p) {
        return p.name.size();
    });

    const std::string lower = lowercase(text);
    std::string out;
    std::size_t done = 0;
    for (std::size_t at = lower.find('@'); at != std::string::npos; at = lower.find('@', at + 1)) {
        // Only an '@' starting a word, not one in an email address.
        if (at > 0 && is_word_char(lower[at - 1])) {
            continue;
        }
        const auto person = std::ranges::find_if(people, [&](const Person& p) {
            const std::size_t end = at + 1 + p.name.size();
            return lower.compare(at + 1, p.name.size(), p.name) == 0 &&
                   (end >= lower.size() || !is_word_char(lower[end]));
        });
        if (person == people.end()) {
            continue;
        }
        const std::size_t end = at + 1 + person->name.size();
        const Color c = person->color;
        out += text.substr(done, at - done);
        if (person->everyone) {
            // Nobody's color: just bold and underlined, as it tags whoever reads it.
            out += std::format("<b><u>{}</u></b>", text.substr(at, end - at));
        } else {
            out += std::format("<b>{}<color=#{:02x}{:02x}{:02x}>{}</color>{}</b>", person->us ? "<u>" : "", c.r, c.g,
                               c.b, text.substr(at, end - at), person->us ? "</u>" : "");
        }
        tags_us = tags_us || person->us;
        done = end;
        at = end - 1;
    }
    out += text.substr(done);
    return out;
}

void Chat::print_art(std::uint64_t id, std::string_view name, std::string_view art) const {
    const std::string time = terminal_.colors() ? std::format("\x1b[90m{}\x1b[0m", timestamp()) : timestamp();
    // All in one print, so lines printed meanwhile by other threads do not end up in the middle of the drawing.
    std::string out = std::format("{} {}:", time, colored_name(id, name));
    const std::string drawing = image::render(art, terminal_.colors());
    for (std::string_view rest = drawing; !rest.empty();) {
        const auto nl = rest.find('\n');
        out += "\n      ";
        out += rest.substr(0, nl);
        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
    }
    terminal_.print(out);
}

void Chat::print_picture(std::uint64_t id, std::string_view name, std::string_view text) const {
    const auto picture = image::parse_picture(text);
    if (!picture) {
        return;
    }
    const std::string time = terminal_.colors() ? std::format("\x1b[90m{}\x1b[0m", timestamp()) : timestamp();
    std::vector<Screen::Frame> frames;
    for (const auto& frame : picture->frames) {
        frames.push_back({frame.mime, frame.base64, frame.delay_ms});
    }
    if (terminal_.show_image(std::format("{} {}:", time, colored_name(id, name)), picture->width, picture->height,
                             frames)) {
        return;
    }
    // Where pictures cannot be shown, drawn with characters: about one per 8 pixels, as wide as a terminal allows.
    const std::size_t cols = std::clamp<std::size_t>(static_cast<std::size_t>(picture->width) / 8, 8, max_art_cols);
    // (An animation: its first frame.)
    if (const auto art = image::to_ascii_data(picture->first, cols, max_art_rows)) {
        print_art(id, name, *art);
    }
}

std::vector<std::string> Chat::peers() const {
    std::vector<std::string> names;
    {
        std::scoped_lock lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            names.push_back(colored_name(id, peer.name));
        }
    }
    std::ranges::sort(names);
    return names;
}

std::vector<Screen::Mention> Chat::mentionable() const {
    std::vector<Screen::Mention> people;
    {
        std::scoped_lock lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            people.push_back({peer.name, terminal_.colors() ? ansi_foreground(color_of_id(id)) : std::string()});
        }
    }
    std::ranges::sort(people, {}, [](const Screen::Mention& m) {
        return lowercase(m.name);
    });
    // Two peers with the same name are tagged the same way.
    const auto dupes = std::ranges::unique(people, {}, [](const Screen::Mention& m) {
        return lowercase(m.name);
    });
    people.erase(dupes.begin(), dupes.end());
    // Tagging everyone is offered first, when there is somebody to tag, and only once if someone is named so.
    std::erase_if(people, [](const Screen::Mention& m) {
        return lowercase(m.name) == everyone_tag;
    });
    if (!people.empty()) {
        people.insert(people.begin(), {std::string(everyone_tag), terminal_.colors() ? "1" : ""});
    }
    return people;
}

std::string Chat::colored_name(std::uint64_t id, std::string_view name) const {
    if (!terminal_.colors()) {
        return std::string(name);
    }
    // Our own name is bold, so it stands out.
    return std::format("\x1b[{}{}m{}\x1b[0m", id == id_ ? "1;" : "", ansi_foreground(color_of_id(id)), name);
}

void Chat::notice(std::string_view text) const {
    if (terminal_.colors()) {
        terminal_.print(std::format("\x1b[90m{} *\x1b[0m {}", timestamp(), text));
    } else {
        terminal_.print(std::format("{} * {}", timestamp(), text));
    }
}

void Chat::send(PacketType type, std::string_view text, bool once) {
    Packet packet;
    packet.type = type;
    packet.sender = id_;
    packet.seq = ++seq_;
    packet.name = name();
    packet.text = std::string(text);
    socket_.broadcast(cipher::scramble(encode(packet)), once);
}

void Chat::run(std::stop_token stop) {
    auto next_heartbeat = clock::now() + heartbeat_interval;
    auto next_refresh = clock::now() + interfaces_refresh_interval;
    while (!stop.stop_requested()) {
        if (auto data = socket_.receive(200ms)) {
            // Older versions send plaintext: still understood, but never sent.
            const auto plain = cipher::unscramble(*data);
            if (auto packet = decode(plain ? *plain : *data); packet && packet->sender != id_) {
                handle(*packet);
            }
        }
        request_missing_chunks();
        const auto now = clock::now();
        if (now >= next_refresh) {
            socket_.refresh_targets();
            next_refresh = now + interfaces_refresh_interval;
        }
        if (now >= next_heartbeat) {
            send(PacketType::Here);
            prune_silent_peers();
            next_heartbeat = now + heartbeat_interval;
        }
        std::scoped_lock lock(hooks_mutex_);
        if (hooks_.tick) {
            hooks_.tick();
        }
    }
}

void Chat::handle(const Packet& packet) {
    enum class Event { None, Joined, Discovered, Renamed, Recolored, Left };
    Event event = Event::None;
    std::string old_name;
    std::uint64_t new_id = 0;
    {
        std::scoped_lock lock(peers_mutex_);
        if (std::ranges::find(old_ids_, packet.sender) != old_ids_.end()) {
            return;
        }
        auto it = peers_.find(packet.sender);
        if (it == peers_.end()) {
            if (packet.type == PacketType::Leave) {
                return;
            }
            it = peers_.emplace(packet.sender, Peer {packet.name, clock::now(), {}}).first;
            event = packet.type == PacketType::Join ? Event::Joined : Event::Discovered;
        } else {
            // Every packet is sent once per broadcast address, so the same one can arrive several times.
            auto& seqs = it->second.recent_seqs;
            if (std::ranges::find(seqs, packet.seq) != seqs.end()) {
                return;
            }
            it->second.last_seen = clock::now();
            if (it->second.name != packet.name) {
                old_name = std::exchange(it->second.name, packet.name);
                event = Event::Renamed;
            }
        }
        auto& seqs = it->second.recent_seqs;
        seqs.push_back(packet.seq);
        if (seqs.size() > dedup_window) {
            seqs.pop_front();
        }
        if (packet.type == PacketType::Leave) {
            // A Leave naming another id is a color change, see set_color().
            const auto next = parse_id(packet.text);
            if (next && *next != packet.sender) {
                Peer peer = std::move(it->second);
                peer.recent_seqs.clear();
                peer.last_seen = clock::now();
                // Its first packet from the new id may have arrived already.
                peers_.try_emplace(*next, std::move(peer));
                new_id = *next;
                event = Event::Recolored;
            } else {
                event = Event::Left;
            }
            peers_.erase(it);
        }
    }

    const std::string who = colored_name(packet.sender, packet.name);
    switch (event) {
    case Event::Joined:
        notice(std::format("{} joined the chat", who));
        break;
    case Event::Discovered:
        notice(std::format("{} is here", who));
        break;
    case Event::Renamed:
        notice(std::format("{} is now known as {}", colored_name(packet.sender, old_name), who));
        break;
    case Event::Recolored:
        notice(std::format("{} changed color to {}", who, colored_name(new_id, packet.name)));
        break;
    case Event::Left:
        notice(std::format("{} left the chat", who));
        break;
    case Event::None:
        break;
    }

    switch (packet.type) {
    case PacketType::Join:
        // Let the newcomer know we are here.
        send(PacketType::Here);
        break;
    case PacketType::Message: {
        if (print_message(packet.sender, packet.name, packet.text)) {
            terminal_.bell();
        }
        history::add({std::time(nullptr), packet.sender, packet.name, packet.text});
        std::scoped_lock lock(hooks_mutex_);
        if (hooks_.message) {
            hooks_.message(packet.sender, packet.name, packet.text);
        }
        break;
    }
    case PacketType::Game: {
        std::scoped_lock lock(hooks_mutex_);
        if (hooks_.packet) {
            hooks_.packet(packet.sender, packet.name, packet.text);
        }
        break;
    }
    case PacketType::Art:
        print_art(packet.sender, packet.name, packet.text);
        break;
    case PacketType::Image:
        print_picture(packet.sender, packet.name, packet.text);
        break;
    case PacketType::Chunk:
        receive_chunk(packet);
        break;
    case PacketType::Resend:
        resend_chunks(packet.text);
        break;
    case PacketType::Here:
    case PacketType::Leave:
        break;
    }
}

void Chat::prune_silent_peers() {
    std::vector<std::string> gone;
    {
        std::scoped_lock lock(peers_mutex_);
        const auto now = clock::now();
        for (auto it = peers_.begin(); it != peers_.end();) {
            if (now - it->second.last_seen > peer_timeout) {
                gone.push_back(colored_name(it->first, it->second.name));
                it = peers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& who : gone) {
        notice(std::format("{} vanished (timed out)", who));
    }
}

} // namespace zchat
