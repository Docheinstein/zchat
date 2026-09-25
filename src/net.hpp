#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zchat::net {

// Initializes the socket library for the lifetime of the object (Winsock on Windows, nothing elsewhere).
class NetworkInit {
public:
    NetworkInit();
    ~NetworkInit();
    NetworkInit(const NetworkInit&) = delete;
    NetworkInit& operator=(const NetworkInit&) = delete;
};

// The IPv4 broadcast addresses (network byte order) to reach every peer on the local networks of this host:
// the limited broadcast 255.255.255.255 plus the directed broadcast of each IPv4 interface.
std::vector<std::uint32_t> broadcast_addresses();

// A UDP socket bound to a port shared with other zchat instances on the same host, which sends to all the
// local broadcast addresses.
class BroadcastSocket {
public:
    explicit BroadcastSocket(std::uint16_t port);
    ~BroadcastSocket();
    BroadcastSocket(const BroadcastSocket&) = delete;
    BroadcastSocket& operator=(const BroadcastSocket&) = delete;

    // Sends the payload to every broadcast target. Thread-safe.
    void broadcast(std::string_view payload);

    // Re-reads the network interfaces, in case they changed. Thread-safe.
    void refresh_targets();

    // Waits up to timeout for a datagram.
    std::optional<std::string> receive(std::chrono::milliseconds timeout);

private:
    std::uintptr_t handle_;
    std::uint16_t port_;
    std::mutex targets_mutex_;
    std::vector<std::uint32_t> targets_;
};

} // namespace zchat::net
