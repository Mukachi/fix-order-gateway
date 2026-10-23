// Tests for include/fixgw/durability.hpp. Spec: docs/design/004-store.md: each
// strategy journals an exchange that recovers exactly; group commit holds the
// outputs of a whole group until its bound; the standby's journal equals the
// local one. The guarantees under crashes are crash_test's (S1, S4).
#include "fix_text.hpp"
#include "peer.hpp"

#include <fixgw/durability.hpp>
#include <fixgw/recovery.hpp>

#include <filesystem>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <unistd.h>

namespace {

using namespace fixgw;
using namespace fixgw::test;

// For the recovered session: replay journals nothing.
struct no_journal {
    bool append(record_kind, session_time, std::string_view, std::string_view) { return false; }
    durability make_durable(session_time) { return durability::failed; }
    durability poll(session_time) { return durability::durable; }
};

// The state a recovery must reproduce.
template <class S>
std::string summary(const S& s) {
    std::string r = std::to_string(static_cast<int>(s.state().state())) + " " + std::to_string(s.state().next_outbound()) +
                    " " + std::to_string(s.state().next_expected()) + "|";
    for (std::uint64_t q = 1; q <= s.store().last(); ++q) r.append(s.store().lookup(q)).append("|");
    return r;
}

// The exchange of recovery_test, over strategy J; returns the session's summary.
template <class J>
std::string exchange(J& j) {
    wire w;
    policy p;
    peer_link l;
    journaled_session<J, wire, policy> us(ours(), j, w, p);
    l.peer.on_connected(at(0));
    us.on_connected(at(0));
    l.pump(us, w, 0);
    for (int i = 0; i < 3; ++i) us.send("D", test::soh("11=O" + std::to_string(i) + "|55=VOD.L|"), at(1));
    l.peer.send("8", test::soh("37=X1|17=E1|150=0|39=0|"), at(2));
    l.peer.send("8", test::soh("37=X2|17=E2|150=0|39=0|"), at(2));
    l.pump(us, w, 2);
    us.on_tick(at(31));
    l.pump(us, w, 31);
    return l.ps.delivered == 3 && w.delivered.size() == 2 ? summary(us) : "exchange incomplete";
}

std::string recovered(const std::string& path) {
    journal_reader r;
    if (!r.open(path)) return "unreadable";
    no_journal nj;
    wire w;
    policy p;
    journaled_session<no_journal, wire, policy> again(ours(), nj, w, p);
    return again.recover(r) ? summary(again) : "replay failed";
}

void each_strategy(const std::string& dir) {
    {
        fsync_each j;
        j.create(dir + "/a.journal", 1, 1u << 20);
        const std::string s = exchange(j);
        check(s != "exchange incomplete" && recovered(dir + "/a.journal") == s, "A fsync each: the exchange recovers exactly");
    }
    {
        group_commit j(4, 500'000);
        j.create(dir + "/b.journal", 1, 1u << 20);
        const std::string s = exchange(j);
        check(s != "exchange incomplete" && recovered(dir + "/b.journal") == s,
              "B group commit (4 records or 0.5 ms): the exchange recovers exactly");
    }
    {
        mapped_journal j;
        j.create(dir + "/c.journal", 1, 1u << 20);
        const std::string s = exchange(j);
        j.close();
        journal_reader r;
        r.open(dir + "/c.journal");
        check(s != "exchange incomplete" && recovered(dir + "/c.journal") == s && r.file_size() == (1u << 20) &&
                  r.valid_end() < r.file_size() && !r.torn(),
              "C mapped: the exchange recovers exactly; the reader stops at the zeros of the unwritten rest");
    }
}

void group_bounds(const std::string& dir) {
    group_commit j(3, 1'000'000);  // 3 records or 1 ms
    j.create(dir + "/g.journal", 1, 1u << 20);
    wire w;
    policy p;
    peer_link l;
    journaled_session<group_commit, wire, policy> us(ours(), j, w, p);
    l.peer.on_connected(at(0));
    us.on_connected(at(0));  // record 1: held (the Logon)
    check(us.holding() && w.sent.empty(), "B: after one record the Logon is held");
    us.poll({999'999, base_ms});
    check(us.holding() && w.sent.empty(), "B: held still just before the time bound");
    us.poll({1'000'000, base_ms + 1});
    check(!us.holding() && !w.sent.empty(), "B: released at the time bound");
    l.pump(us, w, 0);  // the counterparty's Logon: a pending group of its own
    us.poll({5'000'000, base_ms + 5});
    w.sent.clear();
    const session_time t{10'000'000, base_ms + 10};
    us.send("D", test::soh("11=G1|"), t);
    us.send("D", test::soh("11=G2|"), t);
    const bool held_two = w.sent.empty() && us.holding();
    us.send("D", test::soh("11=G3|"), t);
    const auto a = w.sent.find("11=G1"), b = w.sent.find("11=G2"), c = w.sent.find("11=G3");
    check(held_two && !us.holding() && a != std::string::npos && a < b && b < c,
          "B: two orders held, the third fills the group: all three released, in order");
}

void standby(const std::string& dir) {
    const int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    const bool listening = ::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && ::listen(lfd, 1) == 0 &&
                           ::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len) == 0;
    std::uint64_t received = 0;
    std::thread t([&] {
        const int c = ::accept(lfd, nullptr, nullptr);
        journal_file f;
        f.create(dir + "/standby.journal", 1);
        received = run_standby(c, f);
        ::close(c);
    });
    std::string s;
    {
        replicated j;
        j.create(dir + "/d.journal", 1, 1u << 20);
        const bool connected = listening && j.connect_standby(ntohs(addr.sin_port));
        s = connected ? exchange(j) : "not connected";
    }  // closing the connection ends the standby's loop
    t.join();
    ::close(lfd);
    journal_reader local, remote;
    local.open(dir + "/d.journal");
    remote.open(dir + "/standby.journal");
    bool same = local.records().size() == remote.records().size() && received == local.records().size();
    for (std::size_t i = 0; same && i < local.records().size(); ++i) {
        const auto &x = local.records()[i], &y = remote.records()[i];
        same = x.kind == y.kind && x.time.mono_ns == y.time.mono_ns && x.time.utc_ms == y.time.utc_ms && x.payload == y.payload;
    }
    check(s != "not connected" && s != "exchange incomplete" && recovered(dir + "/d.journal") == s,
          "D replicated: the exchange recovers exactly from the local journal");
    check(same && received > 0, "D: the standby's journal has every record the local one has, the same (" +
                                    std::to_string(received) + " records acknowledged)");
    check(recovered(dir + "/standby.journal") == s, "D: the standby's journal alone recovers the same session");
}

// D1 with D: a standby that receives but never acknowledges. Nothing is
// released, and the session fails at the timeout instead of hanging.
void silent_standby(const std::string& dir) {
    const int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    ::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(lfd, 1);
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len);
    std::thread t([&] {
        const int c = ::accept(lfd, nullptr, nullptr);
        char b[4096];
        while (::recv(c, b, sizeof(b), 0) > 0) {
        }
        ::close(c);
    });
    wire w;
    policy p;
    {
        replicated j;
        j.create(dir + "/silent.journal", 1);
        j.connect_standby(ntohs(addr.sin_port), 200);
        journaled_session<replicated, wire, policy> us(ours(), j, w, p);
        us.on_connected(at(0));  // the Logon: never acknowledged
        check(w.sent.empty() && w.disconnects == 1 && us.failed(),
              "D: no acknowledgement within 200 ms: the Logon is not sent, the session fails and disconnects");
    }
    t.join();
    ::close(lfd);
}

}  // namespace

int main() {
    char t[] = "durability_test_XXXXXX";
    const char* d = ::mkdtemp(t);
    if (!d) {
        check(false, "temporary directory");
        return result();
    }
    each_strategy(d);
    group_bounds(d);
    standby(d);
    silent_standby(d);
    std::filesystem::remove_all(d);
    return result();
}
