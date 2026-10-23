// Crash injection (note 004, S1 and S4): our journaled initiator runs in a
// child process and is killed at a chosen point; a new child recovers from the
// journal and finishes. The counterparty, a plain acceptor in the parent,
// records everything it receives. Checked at the end of every run:
//   no MsgSeqNum ever carried two different messages (S1);
//   every order reached the counterparty's application exactly once;
//   the counterparty never saw our sequence go back ("MsgSeqNum too low").
// Power loss is simulated by cutting the journal back to the last point the
// child reported durable (for D, by recovering from the standby's journal).
// Under it C must fail the last check, as its guarantee says; A, B, D must not.
#include "fix_text.hpp"

#include <fixgw/durability.hpp>
#include <fixgw/recovery.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace fixgw;
using fixgw::test::check;

constexpr int orders = 8;
enum class point { none, before_append, mid_append, after_append, after_durable, before_release, mid_release };
enum class kind { A, B, C, D };
const char* name(kind k) { return k == kind::A ? "A" : k == kind::B ? "B" : k == kind::C ? "C" : "D"; }
const char* name(point p) {
    switch (p) {
        case point::before_append: return "before append";
        case point::mid_append: return "mid append";
        case point::after_append: return "after append";
        case point::after_durable: return "after durable";
        case point::before_release: return "before release";
        case point::mid_release: return "mid release";
        default: return "none";
    }
}

session_time now() {
    timespec m, r;
    ::clock_gettime(CLOCK_MONOTONIC, &m);
    ::clock_gettime(CLOCK_REALTIME, &r);
    return {m.tv_sec * 1'000'000'000LL + m.tv_nsec, r.tv_sec * 1000LL + r.tv_nsec / 1'000'000};
}

struct plan {
    point at = point::none;
    int occurrence = 0, seen = 0;
    bool hit(point p) { return p == at && ++seen == occurrence; }
};

// Wraps a strategy with the fail points and reports each durable offset.
template <class Inner>
struct crashing {
    Inner& in;
    plan& pl;
    std::string path;
    int report;  // to the parent: the journal's size each time it is durable
    bool append(record_kind k, session_time t, std::string_view a, std::string_view b) {
        if (pl.hit(point::before_append)) ::_exit(9);
        if (pl.hit(point::mid_append)) {  // half a record reaches the file, then death
            char buf[journal_format::record_overhead + 4096];
            const std::size_t n = encode_record(buf, k, t, a.substr(0, 2048), b.substr(0, 2000));
            const int fd = ::open(path.c_str(), O_WRONLY);
            if (::pwrite(fd, buf, n / 2, static_cast<off_t>(in.end())) < 0) ::_exit(10);
            ::_exit(9);
        }
        const bool ok = in.append(k, t, a, b);
        if (ok && pl.hit(point::after_append)) ::_exit(9);
        return ok;
    }
    durability settled(durability d) {
        if (d == durability::durable) {
            const std::uint64_t end = in.end();  // the file is pre-filled: its size says nothing (D5)
            if (::write(report, &end, sizeof(end)) < 0) ::_exit(10);
            if (pl.hit(point::after_durable)) ::_exit(9);
        }
        return d;
    }
    durability make_durable(session_time t) { return settled(in.make_durable(t)); }
    durability poll(session_time t) { return settled(in.poll(t)); }
};

struct socket_out {
    int fd;
    plan& pl;
    void write(std::string_view b) {
        if (pl.hit(point::before_release)) ::_exit(9);
        if (pl.hit(point::mid_release)) {  // some of the outputs out, then death
            if (::send(fd, b.data(), b.size() / 2, MSG_NOSIGNAL) < 0) ::_exit(10);
            ::_exit(9);
        }
        std::size_t done = 0;
        while (done < b.size()) {
            const ssize_t w = ::send(fd, b.data() + done, b.size() - done, MSG_NOSIGNAL);
            if (w <= 0) ::_exit(11);
            done += static_cast<std::size_t>(w);
        }
    }
    void deliver(std::string_view, bool) {}
    void disconnect() { ::shutdown(fd, SHUT_RDWR); }
};

struct quiet {
    void log(session_log, std::string_view, std::uint64_t) {}
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};

session_config ours() {
    session_config c;
    c.role = session_role::initiator;
    c.sender_comp_id = "CLIENT";
    c.target_comp_id = "VENUE";
    return c;
}

// The child: recover if there is a journal, connect, send the orders not yet
// submitted, then log out. Exits 0 when done; 9 at its fail point.
template <class J>
[[noreturn]] void child_run(J& inner, const std::string& path, bool recovering, int sock, int report, plan pl) {
    crashing<J> j{inner, pl, path, report};
    socket_out out{sock, pl};
    quiet q;
    journaled_session<crashing<J>, socket_out, quiet> us(ours(), j, out, q);
    int sent = 0;
    if (recovering) {
        journal_reader r;
        if (!r.open(path) || !us.recover(r)) ::_exit(12);
        for (const auto& rec : r.records()) sent += rec.kind == record_kind::send;
        us.on_disconnected(now());  // the old connection died with the old process
    }
    us.on_connected(now());
    std::string inbox;
    auto quiet_since = std::chrono::steady_clock::now();
    for (int step = 0; step < 20000; ++step) {
        pollfd p{sock, POLLIN, 0};
        if (::poll(&p, 1, 1) > 0) {
            char b[4096];
            const ssize_t n = ::recv(sock, b, sizeof(b), 0);
            if (n <= 0) ::_exit(13);
            inbox.append(b, static_cast<std::size_t>(n));
            inbox.erase(0, us.on_bytes(inbox.data(), inbox.size(), now()));
            quiet_since = std::chrono::steady_clock::now();
        }
        if (us.state().state() == session_state::active && sent < orders && step % 3 == 0) {
            us.send("D", test::soh(test::order_fields("O" + std::to_string(sent))), now());
            ++sent;
        }
        us.on_tick(now());
        us.poll(now());
        if (us.failed()) ::_exit(14);
        if (sent == orders && us.state().state() == session_state::active && !us.holding() &&
            std::chrono::steady_clock::now() - quiet_since > std::chrono::milliseconds(60)) {
            us.logout("done", now());
            for (int k = 0; k < 200 && us.state().state() != session_state::disconnected; ++k) {
                if (::poll(&p, 1, 2) > 0) {
                    char b[4096];
                    const ssize_t n = ::recv(sock, b, sizeof(b), 0);
                    if (n <= 0) break;
                    inbox.append(b, static_cast<std::size_t>(n));
                    inbox.erase(0, us.on_bytes(inbox.data(), inbox.size(), now()));
                }
                us.poll(now());
            }
            ::_exit(0);
        }
    }
    ::_exit(15);
}

// The counterparty and the record of what it received.
struct peer_sink {
    int fd = -1;
    std::set<std::string> delivered_ids;
    int delivered = 0, duplicates = 0;
    bool too_low = false;
    void write(std::string_view b) {
        if (fd >= 0 && ::send(fd, b.data(), b.size(), MSG_NOSIGNAL) < 0) {
        }
    }
    void disconnect() {
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    }
    void deliver(const message_view<>& m) {
        ++delivered;
        if (!delivered_ids.insert(std::string(m.get(11).value_or(""))).second) ++duplicates;
    }
    void log(session_log, std::string_view what, std::uint64_t) {
        if (what.find("MsgSeqNum too low") != std::string_view::npos) too_low = true;
    }
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};
struct peer_store {
    std::map<std::uint64_t, std::string> m;
    void record(std::uint64_t s, std::string_view b) { m[s] = std::string(b); }
    std::string_view lookup(std::uint64_t s) {
        const auto it = m.find(s);
        return it == m.end() ? std::string_view{} : std::string_view(it->second);
    }
    void reset() { m.clear(); }
};

// A message's content for S1: MsgType and body, without the fields a resend
// legitimately changes (header, PossDup, OrigSendingTime, trailer).
std::string content(const message_view<>& v) {
    std::string c(v.msg_type());
    for (std::size_t i = 0; i < v.size(); ++i) {
        const auto t = v[i].tag;
        if (detail::is_header_tag(t) || detail::is_trailer_tag(t)) continue;
        c.append("|").append(std::to_string(t)).append("=").append(v.value(i));
    }
    return c;
}

struct outcome {
    bool completed = false, conflict = false, too_low = false, resent = false;
    int delivered = 0, duplicates = 0, crashes = 0;
};

// One run: strategy k, killed at `at` (its `occurrence`th time) in child
// generation `gen` (0 the first child, 1 the one that recovers), optionally
// followed by a simulated power loss.
outcome run(kind k, point at, int occurrence, int gen, bool power_loss, const std::string& dir) {
    const std::string path = dir + "/run.journal", standby_path = dir + "/standby.journal";
    std::filesystem::remove(path);
    std::filesystem::remove(standby_path);
    // D's standby, in the parent: one connection per child.
    int lfd = -1;
    std::uint16_t port = 0;
    std::thread standby;
    if (k == kind::D) {
        lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(a);
        ::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(lfd, 4);
        ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        standby = std::thread([lfd, standby_path] {
            journal_file f;
            f.create(standby_path, 1);
            for (;;) {
                const int c = ::accept(lfd, nullptr, nullptr);
                if (c < 0) return;
                run_standby(c, f);
                ::close(c);
            }
        });
    }
    peer_sink ps;
    peer_store pst;
    session_config pc;
    pc.role = session_role::acceptor;
    pc.sender_comp_id = "VENUE";
    pc.target_comp_id = "CLIENT";
    session<peer_sink, peer_store> peer(pc, ps, pst);
    std::map<std::uint64_t, std::string> seen;  // MsgSeqNum -> content, for S1
    outcome o;
    std::uint64_t durable = journal_format::header_size;  // create() makes the header durable
    for (int g = 0; g < 4; ++g) {
        int sv[2], rp[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0 || ::pipe2(rp, O_CLOEXEC | O_NONBLOCK) != 0) break;
        const bool recovering = g > 0;
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::close(sv[0]);
            ::close(rp[0]);
            plan pl;
            if (g == gen) pl = {at, occurrence, 0};
            const auto setup_file = [&](auto& j) {
                if (!recovering) return j.create(path, 1, 64u << 10);
                journal_reader r;
                return r.open(path) && r.cut() && j.open_append(path, r.valid_end(), 64u << 10);
            };
            if (k == kind::A) {
                fsync_each j;
                if (!setup_file(j)) ::_exit(16);
                child_run(j, path, recovering, sv[1], rp[1], pl);
            } else if (k == kind::B) {
                group_commit j(3, 2'000'000);
                if (!setup_file(j)) ::_exit(16);
                child_run(j, path, recovering, sv[1], rp[1], pl);
            } else if (k == kind::C) {
                mapped_journal j;
                bool ok;
                if (!recovering) {
                    ok = j.create(path, 1, 1u << 20);
                } else {
                    journal_reader r;
                    ok = r.open(path) && r.cut() && j.open_append(path, r.valid_end(), 1u << 20);
                }
                if (!ok) ::_exit(16);
                child_run(j, path, recovering, sv[1], rp[1], pl);
            } else {
                replicated j;
                if (!setup_file(j) || !j.connect_standby(port)) ::_exit(16);
                child_run(j, path, recovering, sv[1], rp[1], pl);
            }
        }
        ::close(sv[1]);
        ::close(rp[1]);
        ps.fd = sv[0];
        peer.on_connected(now());
        std::string inbox;
        for (;;) {
            pollfd p[2] = {{sv[0], POLLIN, 0}, {rp[0], POLLIN, 0}};
            ::poll(p, 2, 5);
            std::uint64_t d;
            while (::read(rp[0], &d, sizeof(d)) == sizeof(d)) durable = d;
            if (p[0].revents & (POLLIN | POLLHUP)) {
                char b[4096];
                const ssize_t n = ::recv(sv[0], b, sizeof(b), 0);
                if (n <= 0) break;
                inbox.append(b, static_cast<std::size_t>(n));
                // S1: every complete frame, by its number and content, before the session sees it.
                for (std::size_t at_ = 0;;) {
                    const auto f = frame(inbox.data() + at_, inbox.size() - at_, 1 << 16);
                    if (f.status != frame_status::complete) break;
                    message_view<> v;
                    if (decoder<256>::decode(inbox.data() + at_, f.length, v).ok) {
                        const bool gap_fill = v.msg_type() == "4" && v.get(123) == std::string_view("Y");
                        if (v.get(43) == std::string_view("Y") && v.msg_type() == "D") o.resent = true;
                        const auto seq = detail::parse_uint(v.get(34).value_or(""));
                        if (seq && !gap_fill) {
                            const auto [it, fresh] = seen.emplace(*seq, content(v));
                            if (!fresh && it->second != content(v) && seen.count(*seq)) o.conflict = true;
                        }
                    }
                    at_ += f.length;
                }
                inbox.erase(0, peer.on_bytes(inbox.data(), inbox.size(), now()));
            }
            peer.on_tick(now());
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
        while (true) {  // the last durable reports
            std::uint64_t d;
            if (::read(rp[0], &d, sizeof(d)) != sizeof(d)) break;
            durable = d;
        }
        ::close(sv[0]);
        ::close(rp[0]);
        ps.fd = -1;
        peer.on_disconnected(now());
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (code == 0) {
            o.completed = true;
            break;
        }
        if (code != 9) break;  // a failure of the test itself
        ++o.crashes;
        if (power_loss) {  // everything not durable is lost
            if (k == kind::D) {
                std::filesystem::copy_file(standby_path, path, std::filesystem::copy_options::overwrite_existing);
            } else {  // everything past the durable point gone: zeroed (the file keeps its pre-filled size)
                const std::uint64_t keep = k == kind::C ? journal_format::header_size : durable;
                const int fd = ::open(path.c_str(), O_WRONLY);
                struct stat st;
                if (fd < 0 || ::fstat(fd, &st) != 0) break;
                const std::size_t n = static_cast<std::size_t>(st.st_size) > keep ? static_cast<std::size_t>(st.st_size) - keep : 0;
                const std::string zeros(n, '\0');
                const bool ok = ::pwrite(fd, zeros.data(), n, static_cast<off_t>(keep)) == static_cast<ssize_t>(n);
                ::close(fd);
                if (!ok) break;
            }
        }
    }
    if (k == kind::D) {
        ::shutdown(lfd, SHUT_RDWR);
        ::close(lfd);
        standby.join();
    }
    o.delivered = static_cast<int>(ps.delivered_ids.size());
    o.duplicates = ps.duplicates;
    o.too_low = ps.too_low;
    return o;
}

bool good(const outcome& o) { return o.completed && !o.conflict && !o.too_low && o.delivered == orders && o.duplicates == 0; }

}  // namespace

int main() {
    char t[] = "crash_test_XXXXXX";
    const char* d = ::mkdtemp(t);
    if (!d) {
        check(false, "temporary directory");
        return test::result();
    }
    const std::string dir = d;
    {
        const auto o = run(kind::A, point::none, 0, 0, false, dir);
        check(good(o) && o.crashes == 0, "no crash: every order delivered once, no conflict");
    }
    const point points[] = {point::before_append, point::mid_append, point::after_append,
                            point::after_durable, point::before_release, point::mid_release};
    for (const kind k : {kind::A, kind::B, kind::C, kind::D}) {
        for (const bool power : {false, true}) {
            int runs = 0, ok = 0, crashed = 0, broke = 0, resent = 0;
            std::string first_bad;
            for (const point p : points) {
                if (k == kind::C && p == point::mid_append) continue;  // C appends by copying into memory
                for (int gen = 0; gen < 2; ++gen)
                    for (int occ = 1; occ <= 10; ++occ) {
                        const auto o = run(k, p, occ, gen, power, dir);
                        ++runs;
                        crashed += o.crashes > 0;
                        resent += o.crashes > 0 && o.resent;
                        if (good(o)) ++ok;
                        else {
                            ++broke;
                            if (first_bad.empty())
                                first_bad = std::string(name(p)) + " #" + std::to_string(occ) + " gen " + std::to_string(gen) +
                                            (o.completed ? "" : " (did not complete)") + (o.conflict ? " (MsgSeqNum conflict)" : "") +
                                            (o.too_low ? " (sequence went back)" : "") +
                                            " delivered " + std::to_string(o.delivered) + " dup " + std::to_string(o.duplicates);
                        }
                    }
            }
            const std::string what = std::string(name(k)) + (power ? ", simulated power loss" : ", process killed") + ": " +
                                     std::to_string(runs) + " runs (" + std::to_string(crashed) + " crashed, " +
                                     std::to_string(resent) + " of them with orders resent)";
            if (k == kind::C && power)
                check(broke > 0, "S4 C fails the simulated power loss, as its guarantee says: " + what + ", " +
                                     std::to_string(broke) + " broke, e.g. " + first_bad);
            else
                check(broke == 0, "S1, S4 " + what + (broke == 0 ? ", all correct" : ", " + std::to_string(broke) + " broke; first: " + first_bad));
        }
    }
    std::filesystem::remove_all(dir);
    return test::result();
}
