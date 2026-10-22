// Tests for include/fixgw/recovery.hpp. Spec: docs/design/004-store.md (S2,
// and D1-D3): a journaled session, recovered from its journal, equals the one
// that ran; outputs wait for the journal; only ticks that did something are
// journaled.
#include "fix_text.hpp"

#include <fixgw/recovery.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace fixgw;
using fixgw::test::check;

struct file_journal {
    journal_file f;
    bool fail = false;
    int appends = 0;
    bool append(record_kind k, session_time t, std::string_view a, std::string_view b) {
        if (fail) return false;
        ++appends;
        return f.append(k, t, a, b);
    }
    bool make_durable() { return true; }  // the strategies are task 3's; here the page cache is enough
};

struct wire {
    std::string sent;
    std::vector<std::string> delivered;
    int replayed = 0, disconnects = 0;
    void write(std::string_view b) { sent.append(b); }
    void deliver(std::string_view m, bool r) {
        delivered.emplace_back(m);
        replayed += r;
    }
    void disconnect() { ++disconnects; }
};

struct policy {
    void log(session_log, std::string_view, std::uint64_t) {}
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};

// The counterparty: a plain session (note 003) with a sink that collects.
struct peer_sink {
    std::string sent;
    int delivered = 0;
    void write(std::string_view b) { sent.append(b); }
    void disconnect() {}
    void deliver(const message_view<>&) { ++delivered; }
    void log(session_log, std::string_view, std::uint64_t) {}
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

constexpr std::int64_t base_ms = 1'792'567'800'000;
session_time at(std::int64_t s) { return {s * 1'000'000'000, base_ms + s * 1000}; }

session_config ours() {
    session_config c;
    c.role = session_role::initiator;
    c.sender_comp_id = "CLIENT";
    c.target_comp_id = "VENUE";
    return c;
}
session_config theirs() {
    session_config c;
    c.role = session_role::acceptor;
    c.sender_comp_id = "VENUE";
    c.target_comp_id = "CLIENT";
    return c;
}

using ours_t = journaled_session<file_journal, wire, policy>;

struct link {
    peer_sink ps;
    peer_store pst;
    session<peer_sink, peer_store> peer{theirs(), ps, pst};
    std::string inbox_ours, inbox_peer;
    // Moves everything written each way until both sides are quiet, in
    // chunks of `chunk` bytes (partial and coalesced reads).
    void pump(ours_t& us, wire& w, std::int64_t t, std::size_t chunk = 37) {
        for (int round = 0; round < 50 && (!w.sent.empty() || !ps.sent.empty()); ++round) {
            while (!w.sent.empty()) {
                inbox_peer.append(w.sent, 0, std::min(chunk, w.sent.size()));
                w.sent.erase(0, std::min(chunk, w.sent.size()));
                inbox_peer.erase(0, peer.on_bytes(inbox_peer.data(), inbox_peer.size(), at(t)));
            }
            while (!ps.sent.empty()) {
                inbox_ours.append(ps.sent, 0, std::min(chunk, ps.sent.size()));
                ps.sent.erase(0, std::min(chunk, ps.sent.size()));
                inbox_ours.erase(0, us.on_bytes(inbox_ours.data(), inbox_ours.size(), at(t)));
            }
        }
    }
};

std::size_t count(const journal_reader& r, record_kind k) {
    std::size_t n = 0;
    for (const auto& x : r.records()) n += x.kind == k;
    return n;
}

void recovery(const std::string& dir) {
    const std::string path = dir + "/s2.journal";
    file_journal j;
    j.f.create(path, 1);
    wire w;
    policy p;
    link l;
    ours_t us(ours(), j, w, p);

    l.peer.on_connected(at(0));
    us.on_connected(at(0));
    l.pump(us, w, 0);
    check(us.state().state() == session_state::active && l.peer.state() == session_state::active,
          "a journaled initiator logs on with a plain acceptor");
    for (int i = 0; i < 3; ++i) us.send("D", test::soh("11=O" + std::to_string(i) + "|55=VOD.L|"), at(1));
    l.peer.send("8", test::soh("37=X1|17=E1|150=0|39=0|"), at(2));
    l.peer.send("8", test::soh("37=X2|17=E2|150=0|39=0|"), at(2));
    l.pump(us, w, 2);
    const int appends_before_ticks = j.appends;
    us.on_tick(at(3));  // nothing due
    const bool noop_not_journaled = j.appends == appends_before_ticks;
    us.on_tick(at(31));  // Heartbeat due
    l.pump(us, w, 31);
    const bool heartbeat_journaled = j.appends == appends_before_ticks + 1;
    check(noop_not_journaled && heartbeat_journaled, "D2: a tick that did nothing is not journaled; one that sent a Heartbeat is");
    check(w.delivered.size() == 2 && w.replayed == 0 && l.ps.delivered == 3,
          "outputs released after the journal: both ExecutionReports delivered, the counterparty has the three orders");

    // The crash: the object is gone; only the file remains.
    const std::uint64_t out = us.state().next_outbound(), in = us.state().next_expected();
    const auto st = us.state().state();
    std::vector<std::string> sent_messages;
    for (std::uint64_t s = 1; s <= us.store().last(); ++s) sent_messages.emplace_back(us.store().lookup(s));
    const std::vector<std::string> delivered = w.delivered;
    j.f.close();

    journal_reader r;
    check(r.open(path) && !r.torn() && count(r, record_kind::tick) == 1, "the journal reads back whole, one tick in it");
    file_journal j2;
    j2.f.open_append(path);
    wire w2;
    ours_t again(ours(), j2, w2, p);
    const bool ok = again.recover(r);
    bool same_store = again.store().last() == sent_messages.size();
    for (std::uint64_t s = 1; same_store && s <= sent_messages.size(); ++s)
        same_store = again.store().lookup(s) == sent_messages[s - 1];
    check(ok && again.state().next_outbound() == out && again.state().next_expected() == in && again.state().state() == st &&
              same_store,
          "S2: recovered by replay: same numbers in both directions, same state, every sent message the same");
    check(w2.sent.empty() && w2.delivered == delivered && w2.replayed == 2 && j2.appends == 0,
          "replay writes nothing and journals nothing; application messages passed on again, marked replayed");

    // After recovery the connection is gone; log on again and carry on.
    again.on_disconnected(at(40));
    l.peer.on_disconnected(at(40));
    l.inbox_ours.clear();
    l.inbox_peer.clear();
    l.peer.on_connected(at(41));
    again.on_connected(at(41));
    l.pump(again, w2, 41);
    again.send("D", test::soh("11=AFTER|55=VOD.L|"), at(42));
    l.pump(again, w2, 42);
    check(again.state().state() == session_state::active && l.peer.state() == session_state::active && l.ps.delivered == 4 &&
              again.state().next_outbound() == out + 2,
          "after recovery: logon continues the numbers, the counterparty accepts it and the next order");

    // D1: if the journal fails, nothing is released.
    j2.fail = true;
    const auto sent_before = w2.sent.size();
    again.send("D", test::soh("11=LOST|55=VOD.L|"), at(43));
    check(w2.sent.size() == sent_before && w2.disconnects == 1 && again.failed(),
          "D1: the journal fails: the order is not sent, the connection is closed");
}

}  // namespace

int main() {
    char t[] = "recovery_test_XXXXXX";
    const char* d = ::mkdtemp(t);
    if (!d) {
        check(false, "temporary directory");
        return test::result();
    }
    recovery(d);
    std::filesystem::remove_all(d);
    return test::result();
}
