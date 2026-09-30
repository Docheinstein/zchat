#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
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

// A datagram, and the address (IPv4, network byte order) it came from.
struct Datagram {
    std::string data;
    std::uint32_t from = 0;
};

// A UDP socket bound to a port shared with other zchat instances on the same host, which sends to all the
// local broadcast addresses.
class BroadcastSocket {
public:
    explicit BroadcastSocket(std::uint16_t port);
    ~BroadcastSocket();
    BroadcastSocket(const BroadcastSocket&) = delete;
    BroadcastSocket& operator=(const BroadcastSocket&) = delete;

    // Sends the payload to every broadcast target. Thread-safe. With once, to each network only once: to the
    // broadcast address of each interface, without the limited broadcast (unless there is nothing else), which
    // sends it again on one of them; for big payloads, where that would take twice as long.
    void broadcast(std::string_view payload, bool once = false);

    // Re-reads the network interfaces, in case they changed. Thread-safe.
    void refresh_targets();

    // Waits up to timeout for a datagram.
    std::optional<Datagram> receive(std::chrono::milliseconds timeout);

private:
    std::uintptr_t handle_;
    std::uint16_t port_;
    std::mutex targets_mutex_;
    std::vector<std::uint32_t> targets_;
};

// A TCP connection, for what is too big to broadcast (see Chat::send_picture()). Every wait gives up once stop is
// requested, or after the timeout without progress.
class TcpStream {
public:
    // Connects to an IPv4 address (network byte order) and port.
    static std::optional<TcpStream> connect(std::uint32_t address, std::uint16_t port,
                                            std::chrono::milliseconds timeout);

    explicit TcpStream(std::uintptr_t handle) :
        handle_(handle) {}
    ~TcpStream();
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    bool send_all(std::string_view data, std::chrono::milliseconds timeout, std::stop_token stop = {});
    // Up to a '\n' (not included), at most max bytes.
    std::optional<std::string> read_line(std::size_t max, std::chrono::milliseconds timeout, std::stop_token stop = {});
    // Everything until the other end closes the connection, at most max bytes.
    std::optional<std::string> read_all(std::size_t max, std::chrono::milliseconds timeout, std::stop_token stop = {});

private:
    // Waits until the socket can be read (or written); false on timeout or stop.
    bool wait(bool write, std::chrono::milliseconds timeout, const std::stop_token& stop) const;

    std::uintptr_t handle_;
};

// A TCP socket listening on a port chosen by the system, on every interface.
class TcpListener {
public:
    TcpListener();
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    std::uint16_t port() const {
        return port_;
    }

    // Waits up to timeout for a connection.
    std::optional<TcpStream> accept(std::chrono::milliseconds timeout);

private:
    std::uintptr_t handle_;
    std::uint16_t port_ = 0;
};

} // namespace zchat::net
