// TCP for the session (note 005): non-blocking sockets in both roles, sends
// kept in order when the kernel's buffer is full, a bound past which the
// counterparty is taken to have stopped reading, and reconnect backoff.
// Design: docs/design/005-engine.md
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fixgw {

struct socket_options {
    bool no_delay = true;     // TCP_NODELAY: no Nagle delay on small messages
    int send_buffer = 0;      // SO_SNDBUF; 0 leaves the kernel's default
    int receive_buffer = 0;   // SO_RCVBUF; 0 leaves the kernel's default
};

namespace detail {
inline bool configure(int fd, const socket_options& o) noexcept {
    const int one = 1;
    bool ok = ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK) == 0;
    if (o.no_delay) ok = ok && ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0;
    if (o.send_buffer > 0) ok = ok && ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &o.send_buffer, sizeof(int)) == 0;
    if (o.receive_buffer > 0) ok = ok && ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &o.receive_buffer, sizeof(int)) == 0;
    return ok;
}
inline sockaddr_in address(std::uint32_t ip, std::uint16_t port) noexcept {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    return a;
}
}  // namespace detail

// Listens on `ip:port` (port 0: the kernel picks one); accepts without blocking.
class tcp_listener {
public:
    tcp_listener() = default;
    tcp_listener(const tcp_listener&) = delete;
    tcp_listener& operator=(const tcp_listener&) = delete;
    ~tcp_listener() { close(); }

    bool open(std::uint16_t port, std::uint32_t ip = INADDR_LOOPBACK) {
        close();
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd_ < 0) return false;
        const int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a = detail::address(ip, port);
        socklen_t len = sizeof(a);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(fd_, 16) != 0 ||
            ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) != 0)
            return false;
        port_ = ntohs(a.sin_port);
        return true;
    }
    std::uint16_t port() const noexcept { return port_; }
    int fd() const noexcept { return fd_; }
    // A new connection, configured, or -1 if none is waiting.
    int accept(const socket_options& o = {}) {
        const int c = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) return -1;
        if (!detail::configure(c, o)) {
            ::close(c);
            return -1;
        }
        return c;
    }
    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
};

// Starts a connection to `ip:port` without blocking: the socket, or -1.
inline int connect_start(std::uint16_t port, const socket_options& o = {}, std::uint32_t ip = INADDR_LOOPBACK) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (!detail::configure(fd, o)) {
        ::close(fd);
        return -1;
    }
    const sockaddr_in a = detail::address(ip, port);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    return fd;
}
enum class connect_status { connected, in_progress, failed };
// Whether a connection started by connect_start has completed, without blocking.
inline connect_status connect_check(int fd) {
    pollfd p{fd, POLLOUT, 0};
    const int r = ::poll(&p, 1, 0);
    if (r == 0) return connect_status::in_progress;
    int err = 0;
    socklen_t len = sizeof(err);
    if (r < 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) return connect_status::failed;
    return connect_status::connected;
}

// One established connection. Reads take what has arrived; writes go to the
// kernel at once, and what it will not take is kept, in order, and sent first
// next time. More than `max_pending` kept bytes: the counterparty is not
// reading, and write() says so.
class tcp_connection {
public:
    explicit tcp_connection(std::size_t max_pending = 4u << 20) : max_(max_pending) {}
    tcp_connection(const tcp_connection&) = delete;
    tcp_connection& operator=(const tcp_connection&) = delete;
    ~tcp_connection() { close(); }

    void attach(int fd) {
        close();
        fd_ = fd;
        pending_.clear();
        head_ = 0;
        peak_ = 0;
    }
    bool open() const noexcept { return fd_ >= 0; }
    int fd() const noexcept { return fd_; }

    // Bytes read (> 0), 0 if nothing has arrived, -1 if the connection is
    // closed or failed.
    long read_some(char* p, std::size_t n) {
        for (;;) {
            const ssize_t r = ::recv(fd_, p, n, MSG_DONTWAIT);
            if (r > 0) return static_cast<long>(r);
            if (r == 0) return -1;
            if (errno == EINTR) continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
    }

    // False if the connection failed or the kept bytes passed the bound.
    bool write(std::string_view b) {
        if (fd_ < 0) return false;
        if (pending() == 0) {
            const long sent = send_some(b.data(), b.size());
            if (sent < 0) return false;
            b.remove_prefix(static_cast<std::size_t>(sent));
            if (b.empty()) return true;
        }
        pending_.insert(pending_.end(), b.begin(), b.end());
        if (pending() > peak_) peak_ = pending();
        return pending() <= max_;
    }
    // Sends kept bytes; false if the connection failed.
    bool flush() {
        if (pending() == 0) return true;
        const long sent = send_some(pending_.data() + head_, pending());
        if (sent < 0) return false;
        head_ += static_cast<std::size_t>(sent);
        if (head_ == pending_.size()) {
            pending_.clear();
            head_ = 0;
        }
        return true;
    }
    std::size_t pending() const noexcept { return pending_.size() - head_; }
    std::size_t peak_pending() const noexcept { return peak_; }  // the most ever kept
    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    // Bytes the kernel took (>= 0), or -1 on failure.
    long send_some(const char* p, std::size_t n) {
        std::size_t done = 0;
        while (done < n) {
            const ssize_t w = ::send(fd_, p + done, n - done, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (w > 0) {
                done += static_cast<std::size_t>(w);
                continue;
            }
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            return -1;
        }
        return static_cast<long>(done);
    }
    int fd_ = -1;
    std::vector<char> pending_;
    std::size_t head_ = 0, max_, peak_ = 0;
};

// When the initiator next tries to connect: at once the first time, then
// after `first_ns`, doubling to `max_ns`; reset by a connection that logs on.
class backoff {
public:
    explicit backoff(std::int64_t first_ns = 1'000'000'000, std::int64_t max_ns = 30'000'000'000)
        : first_(first_ns), max_(max_ns), next_(first_ns) {}
    bool due(std::int64_t now_ns) const noexcept { return now_ns >= at_; }
    void failed(std::int64_t now_ns) noexcept {
        at_ = now_ns + next_;
        next_ = next_ * 2 > max_ ? max_ : next_ * 2;
    }
    void reset() noexcept {
        next_ = first_;
        at_ = 0;
    }
    std::int64_t next_delay() const noexcept { return next_; }

private:
    std::int64_t first_, max_, next_, at_ = 0;
};

}  // namespace fixgw
