#include "net.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
// Order matters: the Winsock headers must come before iphlpapi.h.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
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

std::optional<Datagram> BroadcastSocket::receive(std::chrono::milliseconds timeout) {
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
    sockaddr_in from {};
    socklen_t from_size = sizeof from;
    const auto n = ::recvfrom(s, buffer.data(), static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from),
                              &from_size);
    if (n <= 0) {
        return std::nullopt;
    }
    return Datagram {std::string(buffer.data(), static_cast<std::size_t>(n)), from.sin_addr.s_addr};
}

namespace {

    void set_blocking(socket_t s, bool blocking) {
#ifdef _WIN32
        u_long nonblocking = blocking ? 0 : 1;
        ioctlsocket(s, FIONBIO, &nonblocking);
#else
        const int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
#endif
    }

    // Waits up to timeout until s can be read (or written), a slice at a time so a stop is noticed.
    bool wait_socket(socket_t s, bool write, std::chrono::milliseconds timeout, const std::stop_token& stop) {
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + timeout;
        while (!stop.stop_requested()) {
            const auto left = duration_cast<milliseconds>(deadline - steady_clock::now());
            if (left.count() <= 0) {
                return false;
            }
            const auto slice = std::min(left, milliseconds(200));
            fd_set set;
            FD_ZERO(&set);
            FD_SET(s, &set);
            timeval tv {};
            tv.tv_sec = static_cast<long>(slice.count() / 1000);
            tv.tv_usec = static_cast<long>((slice.count() % 1000) * 1000);
            const int rc = ::select(static_cast<int>(s + 1), write ? nullptr : &set, write ? &set : nullptr, nullptr,
                                    &tv);
            if (rc > 0) {
                return true;
            }
            if (rc < 0) {
                return false;
            }
        }
        return false;
    }

#ifdef _WIN32
    constexpr int send_flags = 0;
#else
    // A closed connection must be an error, not a SIGPIPE that ends zchat.
    constexpr int send_flags = MSG_NOSIGNAL;
#endif

} // namespace

std::optional<TcpStream> TcpStream::connect(std::uint32_t address, std::uint16_t port,
                                            std::chrono::milliseconds timeout) {
    const socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == invalid_socket) {
        return std::nullopt;
    }
    TcpStream stream(static_cast<std::uintptr_t>(s));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = address;
    // Not blocking while it connects, so it can time out.
    set_blocking(s, false);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
#ifdef _WIN32
        const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const bool pending = errno == EINPROGRESS;
#endif
        if (!pending || !wait_socket(s, true, timeout, {})) {
            return std::nullopt;
        }
        int error = 0;
        socklen_t size = sizeof error;
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) != 0 || error != 0) {
            return std::nullopt;
        }
    }
    set_blocking(s, true);
    return stream;
}

TcpStream::~TcpStream() {
    if (handle_ != static_cast<std::uintptr_t>(invalid_socket)) {
        close_socket(to_socket(handle_));
    }
}

TcpStream::TcpStream(TcpStream&& other) noexcept :
    handle_(std::exchange(other.handle_, static_cast<std::uintptr_t>(invalid_socket))) {}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) {
        if (handle_ != static_cast<std::uintptr_t>(invalid_socket)) {
            close_socket(to_socket(handle_));
        }
        handle_ = std::exchange(other.handle_, static_cast<std::uintptr_t>(invalid_socket));
    }
    return *this;
}

bool TcpStream::wait(bool write, std::chrono::milliseconds timeout, const std::stop_token& stop) const {
    return wait_socket(to_socket(handle_), write, timeout, stop);
}

bool TcpStream::send_all(std::string_view data, std::chrono::milliseconds timeout, std::stop_token stop) {
    while (!data.empty()) {
        if (!wait(true, timeout, stop)) {
            return false;
        }
        const int size = static_cast<int>(std::min<std::size_t>(data.size(), 1 << 20));
        const auto n = ::send(to_socket(handle_), data.data(), size, send_flags);
        if (n <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

std::optional<std::string> TcpStream::read_line(std::size_t max, std::chrono::milliseconds timeout,
                                                std::stop_token stop) {
    std::string line;
    char c = 0;
    while (line.size() < max) {
        if (!wait(false, timeout, stop) || ::recv(to_socket(handle_), &c, 1, 0) != 1) {
            return std::nullopt;
        }
        if (c == '\n') {
            return line;
        }
        line += c;
    }
    return std::nullopt;
}

std::optional<std::string> TcpStream::read_all(std::size_t max, std::chrono::milliseconds timeout,
                                               std::stop_token stop) {
    std::string out;
    std::array<char, 64 * 1024> buffer;
    while (true) {
        if (!wait(false, timeout, stop)) {
            return std::nullopt;
        }
        const auto n = ::recv(to_socket(handle_), buffer.data(), static_cast<int>(buffer.size()), 0);
        if (n == 0) {
            return out;
        }
        if (n < 0 || out.size() + static_cast<std::size_t>(n) > max) {
            return std::nullopt;
        }
        out.append(buffer.data(), static_cast<std::size_t>(n));
    }
}

TcpListener::TcpListener() {
    const socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == invalid_socket) {
        fail("cannot create TCP socket");
    }
    handle_ = static_cast<std::uintptr_t>(s);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    socklen_t size = sizeof addr;
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || ::listen(s, 16) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&addr), &size) != 0) {
        close_socket(s);
        fail("cannot listen on a TCP port");
    }
    port_ = ntohs(addr.sin_port);
}

TcpListener::~TcpListener() {
    close_socket(to_socket(handle_));
}

std::optional<TcpStream> TcpListener::accept(std::chrono::milliseconds timeout) {
    const socket_t s = to_socket(handle_);
    if (!wait_socket(s, false, timeout, {})) {
        return std::nullopt;
    }
    const socket_t client = ::accept(s, nullptr, nullptr);
    if (client == invalid_socket) {
        return std::nullopt;
    }
    return TcpStream(static_cast<std::uintptr_t>(client));
}


} // namespace zchat::net
