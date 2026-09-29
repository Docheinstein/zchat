#pragma once

#include "color.hpp"
#include "net.hpp"
#include "protocol.hpp"
#include "terminal.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace zchat {

// A chat room made of every zchat instance listening on the same UDP port of the local networks.
// There is no server: every peer broadcasts its packets, and keeps track of the others from what it hears.
class Chat {
public:
    // Without a color, a random one is used.
    Chat(std::uint16_t port, std::string name, std::optional<Color> color, Terminal& terminal);
    ~Chat();
    Chat(const Chat&) = delete;
    Chat& operator=(const Chat&) = delete;

    std::string name() const {
        std::scoped_lock lock(name_mutex_);
        return name_;
    }

    // Changes our name; the others see the change right away.
    void set_name(std::string name);

    Color color() const {
        return color_of_id(id_);
    }

    // Changes our name color; the others see the change right away.
    void set_color(Color color);

    // Formats text in a color.
    std::string paint(Color color, std::string_view text) const;

    // Announces this peer and starts listening.
    void start();

    // Sends a chat line to everybody.
    void say(std::string_view text);

    // Tells the others this peer is leaving and stops listening. Safe to call more than once.
    void stop();

    // The names of the other peers currently in the chat.
    std::vector<std::string> peers() const;

    // Our own name, with its color.
    std::string colored_own_name() const {
        return colored_name(id_, name());
    }

    // Formats a name with its color.
    std::string colored_name(std::uint64_t id, std::string_view name) const;

    // Prints an informational line (joins, leaves, command output).
    void notice(std::string_view text) const;

private:
    using clock = std::chrono::steady_clock;

    struct Peer {
        std::string name;
        clock::time_point last_seen;
        std::deque<std::uint64_t> recent_seqs;
    };

    void run(std::stop_token stop);
    void handle(const Packet& packet);
    void send(PacketType type, std::string_view text = {});
    void print_message(std::uint64_t id, std::string_view name, std::string_view text) const;
    void prune_silent_peers();

    // Changes with the color, see set_color().
    std::atomic<std::uint64_t> id_;
    mutable std::mutex name_mutex_;
    std::string name_;
    Terminal& terminal_;
    net::BroadcastSocket socket_;
    std::atomic<std::uint64_t> seq_ {0};
    std::atomic<bool> stopped_ {false};

    mutable std::mutex peers_mutex_;
    std::map<std::uint64_t, Peer> peers_;
    // Ids we used before changing color: our own late packets from them must not look like another peer.
    std::vector<std::uint64_t> old_ids_;

    std::jthread thread_;
};

} // namespace zchat
