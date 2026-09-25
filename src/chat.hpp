#pragma once

#include "net.hpp"
#include "protocol.hpp"
#include "terminal.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace zchat {

// A chat room made of every zchat instance listening on the same UDP port of the local networks.
// There is no server: every peer broadcasts its packets, and keeps track of the others from what it hears.
class Chat {
public:
    Chat(std::uint16_t port, std::string name, Terminal& terminal);
    ~Chat();
    Chat(const Chat&) = delete;
    Chat& operator=(const Chat&) = delete;

    const std::string& name() const {
        return name_;
    }

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
        return colored_name(id_, name_);
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

    std::uint64_t id_;
    std::string name_;
    Terminal& terminal_;
    net::BroadcastSocket socket_;
    std::atomic<std::uint64_t> seq_ {0};
    std::atomic<bool> stopped_ {false};

    mutable std::mutex peers_mutex_;
    std::map<std::uint64_t, Peer> peers_;

    std::jthread thread_;
};

} // namespace zchat
