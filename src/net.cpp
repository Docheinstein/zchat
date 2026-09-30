#include "net.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

#ifdef _WIN32
// Order matters: the Winsock headers must come before iphlpapi.h.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace zchat::net {

namespace {

#ifdef _WIN32
    using socket_t = SOCKET;
    constexpr socket_t invalid_socket = INVALID_SOCKET;

    std::string last_error() {
        return "error " + std::to_string(WSAGetLastError());
    }

    void close_socket(socket_t s) {
        closesocket(s);
    }
#else
    using socket_t = int;
    constexpr socket_t invalid_socket = -1;

    std::string last_error() {
        return std::strerror(errno);
    }

    void close_socket(socket_t s) {
        ::close(s);
    }
#endif

    socket_t to_socket(std::uintptr_t handle) {
        return static_cast<socket_t>(handle);
    }

    [[noreturn]] void fail(const std::string& what) {
        throw std::runtime_error(what + ": " + last_error());
    }

} // namespace

#ifdef _WIN32
NetworkInit::NetworkInit() {
    WSADATA data;
    if (int rc = WSAStartup(MAKEWORD(2, 2), &data); rc != 0) {
        throw std::runtime_error("WSAStartup failed: error " + std::to_string(rc));
    }
}

NetworkInit::~NetworkInit() {
    WSACleanup();
}

std::vector<std::uint32_t> broadcast_addresses() {
    std::vector<std::uint32_t> result {htonl(INADDR_BROADCAST)};
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) {
        return result;
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family != AF_INET || ua->OnLinkPrefixLength == 0 ||
                ua->OnLinkPrefixLength >= 31) {
                continue;
            }
            const auto* sin = reinterpret_cast<const sockaddr_in*>(ua->Address.lpSockaddr);
            const std::uint32_t addr = ntohl(sin->sin_addr.s_addr);
            const std::uint32_t mask = 0xFFFFFFFFu << (32 - ua->OnLinkPrefixLength);
            result.push_back(htonl(addr | ~mask));
        }
    }
    std::ranges::sort(result);
    result.erase(std::ranges::unique(result).begin(), result.end());
    return result;
}
#else
NetworkInit::NetworkInit() = default;
NetworkInit::~NetworkInit() = default;

std::vector<std::uint32_t> broadcast_addresses() {
    std::vector<std::uint32_t> result {htonl(INADDR_BROADCAST)};
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        return result;
    }
    for (ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET || !(ifa->ifa_flags & IFF_UP) ||
            !(ifa->ifa_flags & IFF_BROADCAST) || !ifa->ifa_broadaddr) {
            continue;
        }
        result.push_back(reinterpret_cast<const sockaddr_in*>(ifa->ifa_broadaddr)->sin_addr.s_addr);
    }
    freeifaddrs(list);
    std::ranges::sort(result);
    result.erase(std::ranges::unique(result).begin(), result.end());
    return result;
}
#endif

BroadcastSocket::BroadcastSocket(std::uint16_t port) :
    port_(port) {
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == invalid_socket) {
        fail("cannot create socket");
    }
    handle_ = static_cast<std::uintptr_t>(s);

    const int on = 1;
    const auto* opt = reinterpret_cast<const char*>(&on);
    // Several zchat instances on the same host must all be able to bind the port; broadcasts reach every one.
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, opt, sizeof on) != 0) {
        close_socket(s);
        fail("cannot set SO_REUSEADDR");
    }
    if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, opt, sizeof on) != 0) {
        close_socket(s);
        fail("cannot set SO_BROADCAST");
    }

    // Room for the burst of datagrams of a big picture (see Chat::send_picture()); fine if the system gives less.
    const int buffer_bytes = 16 * 1024 * 1024;
    const auto* buffer_opt = reinterpret_cast<const char*>(&buffer_bytes);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, buffer_opt, sizeof buffer_bytes);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, buffer_opt, sizeof buffer_bytes);

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        close_socket(s);
        fail("cannot bind UDP port " + std::to_string(port));
    }
    targets_ = broadcast_addresses();
}

BroadcastSocket::~BroadcastSocket() {
    close_socket(to_socket(handle_));
}

void BroadcastSocket::refresh_targets() {
    auto targets = broadcast_addresses();
    std::scoped_lock lock(targets_mutex_);
    targets_ = std::move(targets);
}

void BroadcastSocket::broadcast(std::string_view payload, bool once) {
    std::vector<std::uint32_t> targets;
    {
        std::scoped_lock lock(targets_mutex_);
        targets = targets_;
    }
    if (once && targets.size() > 1) {
        std::erase(targets, htonl(INADDR_BROADCAST));
    }
    for (std::uint32_t target : targets) {
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = target;
        // Failures on one interface (e.g. no route for 255.255.255.255) must not stop the others.
        ::sendto(to_socket(handle_), payload.data(), static_cast<int>(payload.size()), 0,
                 reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    }
}

std::optional<std::string> BroadcastSocket::receive(std::chrono::milliseconds timeout) {
    const socket_t s = to_socket(handle_);
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(s, &read_set);
    timeval tv {};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    if (::select(static_cast<int>(s + 1), &read_set, nullptr, nullptr, &tv) <= 0) {
        return std::nullopt;
    }
    // Room for the biggest datagram there can be: an Art drawing with a lot of detail can take more than 8 KB, and a
    // datagram too big for the buffer is lost.
    thread_local std::array<char, 65536> buffer;
    const auto n = ::recvfrom(s, buffer.data(), static_cast<int>(buffer.size()), 0, nullptr, nullptr);
    if (n <= 0) {
        return std::nullopt;
    }
    return std::string(buffer.data(), static_cast<std::size_t>(n));
}

} // namespace zchat::net
