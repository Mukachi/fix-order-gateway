// Tests for include/fixgw/transport.hpp. Spec: docs/design/005-engine.md (S1,
// S4): sessions over real TCP on loopback, with socket buffers forced small so
// reads and writes split anywhere; the bound on a counterparty that does not
// read; reconnect with backoff.
#include "fix_text.hpp"

#include <fixgw/recovery.hpp>
#include <fixgw/session.hpp>
#include <fixgw/transport.hpp>

#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace fixgw;
using fixgw::test::check;

session_time now() {
    timespec m, r;
    ::clock_gettime(CLOCK_MONOTONIC, &m);
    ::clock_gettime(CLOCK_REALTIME, &r);
    return {m.tv_sec * 1'000'000'000LL + m.tv_nsec, r.tv_sec * 1000LL + r.tv_nsec / 1'000'000};
}

// The journal is note 004's; here the page cache would do, and memory is enough.
struct memory_journal {
    bool append(record_kind, session_time, std::string_view, std::string_view) { return true; }
    durability make_durable(session_time) { return durability::durable; }
    durability poll(session_time) { return durability::durable; }
};

// Our side's downstream: the connection, and the application messages.
struct conn_out {
    tcp_connection& c;
    std::vector<std::string> delivered;
    bool failed = false;
    void write(std::string_view b) { failed = !c.write(b) || failed; }
    void deliver(std::string_view m, bool) { delivered.emplace_back(m); }
    void disconnect() { c.close(); }
};
struct yes {
    void log(session_log, std::string_view, std::uint64_t) {}
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};

// The counterparty: a plain session writing to its connection.
struct their_sink {
    tcp_connection& c;
    std::vector<std::string> ids;
    void write(std::string_view b) { c.write(b); }
    void disconnect() { c.close(); }
    void deliver(const message_view<>& m) { ids.emplace_back(m.get(11).value_or("")); }
    std::vector<std::string> logs;
    void log(session_log, std::string_view w, std::uint64_t seq) { logs.push_back(std::string(w) + " " + std::to_string(seq)); }
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};
struct their_store {
    std::map<std::uint64_t, std::string> m;
    void record(std::uint64_t s, std::string_view b) { m[s] = std::string(b); }
    std::string_view lookup(std::uint64_t s) {
        const auto it = m.find(s);
        return it == m.end() ? std::string_view{} : std::string_view(it->second);
    }
    void reset() { m.clear(); }
};

session_config initiator() {
    session_config c;
    c.role = session_role::initiator;
    c.sender_comp_id = "CLIENT";
    c.target_comp_id = "VENUE";
    return c;
}
session_config acceptor() {
    session_config c;
    c.role = session_role::acceptor;
    c.sender_comp_id = "VENUE";
    c.target_comp_id = "CLIENT";
    return c;
}

using ours_t = journaled_session<memory_journal, conn_out, yes>;
using theirs_t = session<their_sink, their_store>;

// Reads each side in 97-byte pieces into its inbox and feeds its session,
// flushes both, until `done` or 10 s pass.
template <class Done>
bool pump(ours_t& us, tcp_connection& uc, std::string& ui, theirs_t& them, tcp_connection& tc, std::string& ti,
          Done done) {
    char b[97];
    const std::int64_t give_up = now().mono_ns + 10'000'000'000;
    while (now().mono_ns < give_up) {
        if (done()) return true;
        long n;
        while (uc.open() && (n = uc.read_some(b, sizeof(b))) > 0) {
            ui.append(b, static_cast<std::size_t>(n));
            ui.erase(0, us.on_bytes(ui.data(), ui.size(), now()));
        }
        while (tc.open() && (n = tc.read_some(b, sizeof(b))) > 0) {
            ti.append(b, static_cast<std::size_t>(n));
            ti.erase(0, them.on_bytes(ti.data(), ti.size(), now()));
        }
        uc.flush();
        tc.flush();
    }
    return done();
}

void s1_and_s4() {
    // Buffers far below a burst, so writes go partial; not below loopback's
    // ~64 KB segment, or TCP paces the transfer by its zero-window probes.
    // Reads take 97 bytes at a time, so they split anywhere.
    socket_options small;
    small.send_buffer = small.receive_buffer = 128 * 1024;
    tcp_listener l;
    check(l.open(0), "listener on loopback, an ephemeral port");
    tcp_connection uc, tc;
    const int cfd = connect_start(l.port(), small);
    int afd = -1;
    for (int k = 0; k < 100'000 && afd < 0; ++k) afd = l.accept(small);
    for (int k = 0; k < 100'000 && connect_check(cfd) == connect_status::in_progress; ++k) {
    }
    check(cfd >= 0 && afd >= 0 && connect_check(cfd) == connect_status::connected, "a connection, non-blocking, both ends");
    uc.attach(cfd);
    tc.attach(afd);

    memory_journal j;
    conn_out out{uc, {}, false};
    yes y;
    ours_t us(initiator(), j, out, y);
    their_sink ts{tc, {}, {}};
    their_store tst;
    theirs_t them(acceptor(), ts, tst);
    std::string ui, ti;
    them.on_connected(now());
    us.on_connected(now());
    const bool logged_on = pump(us, uc, ui, them, tc, ti, [&] {
        return us.state().state() == session_state::active && them.state() == session_state::active;
    });
    check(logged_on, "logon over TCP");

    constexpr int n = 20'000;  // ~2.4 MB: more than the kernel's buffers (twice what is asked, each end) can absorb
    for (int i = 0; i < n; ++i) us.send("D", test::soh("11=O" + std::to_string(i) + "|55=VOD.L|54=1|38=100|40=1|"), now());
    const bool all_in = pump(us, uc, ui, them, tc, ti, [&] { return ts.ids.size() == n; });
    bool order = all_in;
    for (int i = 0; order && i < n; ++i) order = ts.ids[static_cast<std::size_t>(i)] == "O" + std::to_string(i);
    check(order && uc.peak_pending() > 0 && !out.failed,
          "S1: " + std::to_string(n) + " orders in a burst through 128 KB buffers, read 97 bytes at a time: all arrive, intact, in order (up to " +
              std::to_string(uc.peak_pending()) + " bytes kept for the kernel)");
    for (int i = 0; i < n; ++i) them.send("8", test::soh("37=X" + std::to_string(i) + "|17=E" + std::to_string(i) + "|150=0|39=0|"), now());
    const bool back = pump(us, uc, ui, them, tc, ti, [&] { return out.delivered.size() == n; });
    bool reports = back;
    for (int i = 0; reports && i < n; ++i)
        reports = out.delivered[static_cast<std::size_t>(i)].find("\x01" "37=X" + std::to_string(i) + "\x01") != std::string::npos;
    check(reports && tc.peak_pending() > 0, "S1: " + std::to_string(n) + " ExecutionReports the other way: all, in order");

    // S4: the counterparty drops the connection; we reconnect and log on again.
    const std::uint64_t next_before = us.state().next_outbound();
    tc.close();
    them.on_disconnected(now());
    char b[64];
    long r = 0;
    for (int k = 0; k < 100'000 && (r = uc.read_some(b, sizeof(b))) == 0; ++k) {
    }
    check(r < 0, "the drop is seen as a closed connection");
    us.on_disconnected(now());
    uc.close();
    backoff bo;
    check(bo.due(now().mono_ns), "S4: the first reconnect is due at once");
    const int c2 = connect_start(l.port(), small);
    int a2 = -1;
    for (int k = 0; k < 100'000 && a2 < 0; ++k) a2 = l.accept(small);
    for (int k = 0; k < 100'000 && connect_check(c2) == connect_status::in_progress; ++k) {
    }
    uc.attach(c2);
    tc.attach(a2);
    them.on_connected(now());
    us.on_connected(now());
    const bool again = pump(us, uc, ui, them, tc, ti, [&] {
        return us.state().state() == session_state::active && them.state() == session_state::active;
    });
    us.send("D", test::soh("11=AFTER|55=VOD.L|54=1|38=100|40=1|"), now());
    const bool after = pump(us, uc, ui, them, tc, ti, [&] { return ts.ids.size() == n + 1; });
    check(again && after && ts.ids.back() == "AFTER" && us.state().next_outbound() == next_before + 2,
          "S4: reconnected, logged on with the numbers continuing, the next order delivered");
    bo.reset();

    // Connecting to nothing fails, and the backoff spaces the attempts.
    l.close();
    const int c3 = connect_start(l.port(), small);
    connect_status st = connect_status::in_progress;
    for (int k = 0; k < 100'000 && c3 >= 0 && (st = connect_check(c3)) == connect_status::in_progress; ++k) {
    }
    if (c3 >= 0) ::close(c3);
    check(c3 < 0 || st == connect_status::failed, "S4: nothing listening: the connect fails");
}

void backoff_schedule() {
    backoff b(1'000, 8'000);
    bool ok = b.due(0);
    b.failed(0);
    ok = ok && !b.due(999) && b.due(1'000);
    b.failed(1'000);
    ok = ok && !b.due(2'999) && b.due(3'000);
    b.failed(3'000);
    b.failed(7'000);
    ok = ok && b.next_delay() == 8'000;  // capped
    b.reset();
    ok = ok && b.due(0) && b.next_delay() == 1'000;
    check(ok, "backoff: at once, then 1, 2, 4 ... capped at the maximum; reset by a logon");
}

void not_reading() {
    tcp_listener l;
    l.open(0);
    socket_options small;
    small.send_buffer = small.receive_buffer = 4096;
    const int c = connect_start(l.port(), small);
    int a = -1;
    for (int k = 0; k < 100'000 && a < 0; ++k) a = l.accept(small);
    for (int k = 0; k < 100'000 && connect_check(c) == connect_status::in_progress; ++k) {
    }
    tcp_connection conn(64u << 10);
    conn.attach(c);
    const std::string chunk(1000, 'x');
    int writes = 0;
    while (conn.write(chunk) && writes < 10'000) ++writes;
    check(writes < 10'000 && conn.pending() > (64u << 10),
          "a counterparty that does not read: past 64 KB kept, write says so (after " + std::to_string(writes) + " writes)");
    ::close(a);
}

}  // namespace

int main() {
    s1_and_s4();
    backoff_schedule();
    not_reading();
    return test::result();
}
