// The four durability strategies of note 004 (A to D), each a journal writer
// that says when an appended record is as safe as it promises. Design:
// docs/design/004-store.md
//
// Every strategy:
//   bool append(record_kind, session_time, std::string_view a, std::string_view b);
//   durability make_durable(session_time now);  after an append
//   durability poll(session_time now);           between inputs (B's time bound)
#pragma once

#include <vector>

#include <algorithm>

#include <fixgw/journal.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace fixgw {

enum class durability { durable, pending, failed };

// A: each record written, then fdatasync, before anything it caused is
// released. Survives power loss where the device's write cache does.
class fsync_each {
public:
    // prefill: D5 (journal.hpp); 0 grows the file with each record.
    bool create(const std::string& path, std::uint32_t epoch, std::size_t prefill = 0) {
        return f_.create(path, epoch, prefill);
    }
    bool open_append(const std::string& path, std::size_t valid_end, std::size_t prefill = 0) {
        return f_.open_append(path, valid_end, prefill);
    }
    std::size_t end() const noexcept { return f_.end(); }
    bool append(record_kind k, session_time t, std::string_view a, std::string_view b) { return f_.append(k, t, a, b); }
    durability make_durable(session_time) { return ::fdatasync(f_.fd()) == 0 ? durability::durable : durability::failed; }
    durability poll(session_time) { return durability::durable; }

private:
    journal_file f_;
};

// B: records written as they come; one fdatasync for a group, when it holds
// `max_records` or its first record has waited `max_wait_ns`. Until then the
// outputs of the whole group stay held. Survives power loss as A.
class group_commit {
public:
    group_commit(std::size_t max_records, std::int64_t max_wait_ns) : max_records_(max_records), max_wait_ns_(max_wait_ns) {}
    // prefill: D5 (journal.hpp); 0 grows the file with each record.
    bool create(const std::string& path, std::uint32_t epoch, std::size_t prefill = 0) {
        return f_.create(path, epoch, prefill);
    }
    bool open_append(const std::string& path, std::size_t valid_end, std::size_t prefill = 0) {
        return f_.open_append(path, valid_end, prefill);
    }
    std::size_t end() const noexcept { return f_.end(); }
    bool append(record_kind k, session_time t, std::string_view a, std::string_view b) {
        if (!f_.append(k, t, a, b)) return false;
        if (pending_++ == 0) first_ns_ = t.mono_ns;
        return true;
    }
    durability make_durable(session_time t) {
        return pending_ >= max_records_ || t.mono_ns - first_ns_ >= max_wait_ns_ ? flush() : durability::pending;
    }
    durability poll(session_time t) {
        if (pending_ == 0) return durability::durable;
        return t.mono_ns - first_ns_ >= max_wait_ns_ ? flush() : durability::pending;
    }
    std::size_t pending() const noexcept { return pending_; }

private:
    durability flush() {
        pending_ = 0;
        return ::fdatasync(f_.fd()) == 0 ? durability::durable : durability::failed;
    }
    journal_file f_;
    std::size_t max_records_, pending_ = 0;
    std::int64_t max_wait_ns_, first_ns_ = 0;
};

// C: records copied into a memory-mapped file of fixed capacity, never
// flushed on the path. The page cache keeps them if the process dies; a power
// loss or kernel crash may lose any of them. The unwritten rest of the file is
// zeros, which the reader takes as the end.
class mapped_journal {
public:
    mapped_journal() = default;
    mapped_journal(const mapped_journal&) = delete;
    mapped_journal& operator=(const mapped_journal&) = delete;
    ~mapped_journal() { close(); }

    // Pre-filled (note 008's amendment, as D5 does for A): the file's blocks
    // written at creation, so no write through the mapping makes the file
    // system allocate them on the order path (on ext4 a stall of ~150 us
    // about every megabyte of journal), and the mapping faulted in.
    bool create(const std::string& path, std::uint32_t epoch, std::size_t capacity) {
        if (!map(path, O_CREAT | O_EXCL, capacity, true)) return false;
        std::memcpy(base_, journal_format::magic.data(), journal_format::magic.size());
        std::memcpy(base_ + journal_format::magic.size(), &epoch, 4);
        at_ = journal_format::header_size;
        return true;
    }
    // Continues after the last complete record (`valid_end` from the reader,
    // after it cut the file).
    bool open_append(const std::string& path, std::size_t valid_end, std::size_t capacity) {
        if (!map(path, 0, capacity, false)) return false;
        at_ = valid_end;
        return true;
    }
    void close() {
        if (base_) ::munmap(base_, capacity_);
        if (fd_ >= 0) ::close(fd_);
        base_ = nullptr;
        fd_ = -1;
    }
    bool append(record_kind k, session_time t, std::string_view a, std::string_view b) {
        const std::size_t n = journal_format::record_overhead + a.size() + b.size();
        if (a.size() + b.size() > journal_format::max_payload || at_ + n > capacity_) return false;
        encode_record(base_ + at_, k, t, a, b);
        at_ += n;
        return true;
    }
    durability make_durable(session_time) { return durability::durable; }
    durability poll(session_time) { return durability::durable; }
    std::size_t end() const noexcept { return at_; }

private:
    // `prefill`: the whole capacity written with zeros first (a new file).
    bool map(const std::string& path, int flags, std::size_t capacity, bool prefill) {
        close();
        fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC | flags, 0644);
        if (fd_ < 0 || ::ftruncate(fd_, static_cast<off_t>(capacity)) != 0) return false;
        if (prefill) {
            const std::vector<char> zeros(1u << 20, 0);
            for (std::size_t o = 0; o < capacity; o += zeros.size()) {
                const std::size_t n = std::min(zeros.size(), capacity - o);
                if (::pwrite(fd_, zeros.data(), n, static_cast<off_t>(o)) != static_cast<ssize_t>(n)) return false;
            }
            if (::fdatasync(fd_) != 0) return false;
        }
        void* m = ::mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd_, 0);
        if (m == MAP_FAILED) return false;
        base_ = static_cast<char*>(m);
        capacity_ = capacity;
        return true;
    }
    int fd_ = -1;
    char* base_ = nullptr;
    std::size_t capacity_ = 0, at_ = 0;
};

namespace detail {
inline bool send_all(int fd, const char* p, std::size_t n) {
    while (n > 0) {
        const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}
inline bool recv_all(int fd, char* p, std::size_t n) {
    while (n > 0) {
        const ssize_t r = ::recv(fd, p, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}
}  // namespace detail

// D: each record written locally (no fdatasync) and sent to a standby, which
// appends it to its own journal and acknowledges; released once acknowledged.
// Survives the loss of this machine if the standby is on another (and has
// written the record); on one machine it measures the protocol, not a network.
// No acknowledgement within the timeout is a failure: nothing is released and
// the session disconnects (D1), rather than waiting on a dead standby.
class replicated {
public:
    replicated() = default;
    replicated(const replicated&) = delete;
    replicated& operator=(const replicated&) = delete;
    ~replicated() {
        if (sock_ >= 0) ::close(sock_);
    }
    // prefill: D5 (journal.hpp); 0 grows the file with each record.
    bool create(const std::string& path, std::uint32_t epoch, std::size_t prefill = 0) {
        return f_.create(path, epoch, prefill);
    }
    bool open_append(const std::string& path, std::size_t valid_end, std::size_t prefill = 0) {
        return f_.open_append(path, valid_end, prefill);
    }
    std::size_t end() const noexcept { return f_.end(); }
    // Connects to a standby listening on 127.0.0.1:`port`.
    bool connect_standby(std::uint16_t port, std::int64_t ack_timeout_ms = 1000) {
        sock_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (sock_ < 0) return false;
        const int one = 1;
        ::setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        timeval tv{static_cast<time_t>(ack_timeout_ms / 1000), static_cast<suseconds_t>(ack_timeout_ms % 1000 * 1000)};
        ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::connect(sock_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    }
    bool append(record_kind k, session_time t, std::string_view a, std::string_view b) {
        if (!f_.append(k, t, a, b)) return false;
        const std::string_view r = f_.last_record();
        if (!detail::send_all(sock_, r.data(), r.size())) return false;
        ++sent_;
        return true;
    }
    // Waits for the standby to acknowledge every record sent so far; a timeout
    // (SO_RCVTIMEO) or a closed connection is a failure.
    durability make_durable(session_time) {
        while (acked_ < sent_) {
            std::uint64_t n;
            if (!detail::recv_all(sock_, reinterpret_cast<char*>(&n), sizeof(n))) return durability::failed;
            acked_ = n;
        }
        return durability::durable;
    }
    durability poll(session_time) { return durability::durable; }

private:
    journal_file f_;
    int sock_ = -1;
    std::uint64_t sent_ = 0, acked_ = 0;
};

// The standby side of D: reads records from a connection, checks each one,
// appends it to its own journal, and acknowledges with the count received so
// far (u64). Returns when the connection closes or a record fails its check.
inline std::uint64_t run_standby(int conn, journal_file& out) {
    std::uint64_t count = 0;
    std::string buf(journal_format::record_overhead + journal_format::max_payload, '\0');
    for (;;) {
        if (!detail::recv_all(conn, buf.data(), 4)) break;
        std::uint32_t len;
        std::memcpy(&len, buf.data(), 4);
        if (len > journal_format::max_payload) break;
        const std::size_t rest = journal_format::record_overhead - 4 + len;
        if (!detail::recv_all(conn, buf.data() + 4, rest)) break;
        const std::size_t body = journal_format::record_overhead - 4 + len;
        std::uint32_t crc;
        std::memcpy(&crc, buf.data() + body, 4);
        if (crc != detail::crc32c(0, buf.data(), body)) break;
        const char* r = buf.data();
        std::int64_t mono, utc;
        std::memcpy(&mono, r + 5, 8);
        std::memcpy(&utc, r + 13, 8);
        if (!out.append(static_cast<record_kind>(static_cast<unsigned char>(r[4])), {mono, utc},
                        std::string_view(r + 21, len)))
            break;
        ++count;
        if (!detail::send_all(conn, reinterpret_cast<const char*>(&count), sizeof(count))) break;
    }
    return count;
}

}  // namespace fixgw
