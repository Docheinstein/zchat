#include "chat.hpp"

#include "text.hpp"

#include <algorithm>
#include <array>
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

    std::optional<std::uint64_t> parse_id(std::string_view s) {
        std::uint64_t id = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), id, 16);
        if (s.empty() || ec != std::errc {} || ptr != s.data() + s.size() || id == 0) {
            return std::nullopt;
        }
        return id;
    }

    std::string timestamp() {
        const std::time_t now = std::time(nullptr);
        std::tm local {};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        return std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
    }

} // namespace

Chat::Chat(std::uint16_t port, std::string name, std::optional<Color> color, Terminal& terminal) :
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
    send(PacketType::Leave);
    thread_.request_stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Chat::say(std::string_view text) {
    const std::string clean = text::sanitize(text, max_text_bytes);
    send(PacketType::Message, clean);
    print_message(id_, name(), clean);
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

void Chat::print_message(std::uint64_t id, std::string_view name, std::string_view text) const {
    const std::string time = terminal_.colors() ? std::format("\x1b[90m{}\x1b[0m", timestamp()) : timestamp();
    terminal_.print(std::format("{} {}: {}", time, colored_name(id, name), text));
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

void Chat::send(PacketType type, std::string_view text) {
    Packet packet;
    packet.type = type;
    packet.sender = id_;
    packet.seq = ++seq_;
    packet.name = name();
    packet.text = std::string(text);
    socket_.broadcast(encode(packet));
}

void Chat::run(std::stop_token stop) {
    auto next_heartbeat = clock::now() + heartbeat_interval;
    auto next_refresh = clock::now() + interfaces_refresh_interval;
    while (!stop.stop_requested()) {
        if (auto data = socket_.receive(200ms)) {
            if (auto packet = decode(*data); packet && packet->sender != id_) {
                handle(*packet);
            }
        }
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
    case PacketType::Message:
        print_message(packet.sender, packet.name, packet.text);
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
