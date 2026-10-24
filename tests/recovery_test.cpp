// Tests for include/fixgw/recovery.hpp. Spec: docs/design/004-store.md (S2,
// and D1-D3): a journaled session, recovered from its journal, equals the one
// that ran; outputs wait for the journal; only ticks that did something are
// journaled. Note 008's amendment: a backlog read at once is journaled in
// slices of at most max_frame bytes.
#include "fix_text.hpp"
#include "peer.hpp"

#include <fixgw/recovery.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace fixgw;
using fixgw::test::check;
using namespace fixgw::test;

struct file_journal {
    journal_file f;
    bool fail = false;
    int appends = 0;
    bool append(record_kind k, session_time t, std::string_view a, std::string_view b) {
        if (fail) return false;
        ++appends;
        return f.append(k, t, a, b);
    }
    durability make_durable(session_time) { return durability::durable; }  // the page cache: enough here
    durability poll(session_time) { return durability::durable; }
};

using ours_t = journaled_session<file_journal, wire, policy>;

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
    peer_link l;
    ours_t us(ours(), j, w, p);

    l.peer.on_connected(at(0));
    us.on_connected(at(0));
    l.pump(us, w, 0);
    check(us.state().state() == session_state::active && l.peer.state() == session_state::active,
          "a journaled initiator logs on with a plain acceptor");
    for (int i = 0; i < 3; ++i) us.send("D", test::soh(test::order_fields("O" + std::to_string(i))), at(1));
    l.peer.send("8", test::soh(test::report_fields("O1", "E1")), at(2));
    l.peer.send("8", test::soh(test::report_fields("O2", "E2")), at(2));
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
    j2.f.open_append(path, r.valid_end());
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
    again.send("D", test::soh(test::order_fields("AFTER")), at(42));
    l.pump(again, w2, 42);
    check(again.state().state() == session_state::active && l.peer.state() == session_state::active && l.ps.delivered == 4 &&
              again.state().next_outbound() == out + 2,
          "after recovery: logon continues the numbers, the counterparty accepts it and the next order");

    // D1: if the journal fails, nothing is released.
    j2.fail = true;
    const auto sent_before = w2.sent.size();
    again.send("D", test::soh(test::order_fields("LOST")), at(43));
    check(w2.sent.size() == sent_before && w2.disconnects == 1 && again.failed(),
          "D1: the journal fails: the order is not sent, the connection is closed");
}

// Note 008's amendment: over 2 MB of reports handed to the session in one
// call (a backlog read at once behind a slow journal) is taken whole, as
// records of at most max_frame bytes (the journal's max_payload is 1 MB), and
// replays to the same state.
void backlog(const std::string& dir) {
    const std::string path = dir + "/backlog.journal";
    file_journal j;
    j.f.create(path, 1);
    wire w;
    policy p;
    peer_link l;
    ours_t us(ours(), j, w, p);
    l.peer.on_connected(at(0));
    us.on_connected(at(0));
    l.pump(us, w, 0, 1 << 16);
    constexpr int n = 15'000;
    for (int i = 0; i < n; ++i) us.send("D", test::soh(test::order_fields("B" + std::to_string(i))), at(1));
    l.pump(us, w, 1, 1 << 16);
    for (int i = 0; i < n; ++i) l.peer.send("8", test::soh(test::report_fields("B" + std::to_string(i), "X" + std::to_string(i))), at(2));
    const std::string all = l.ps.sent;
    l.ps.sent.clear();
    const std::size_t used = us.on_bytes(all.data(), all.size(), at(2));
    check(all.size() > (2u << 20) && used == all.size() && !us.failed() && w.delivered.size() == static_cast<std::size_t>(n),
          "a backlog over 2 MB in one call: taken whole, every report delivered, the journal has not failed");
    j.f.close();

    journal_reader r;
    std::size_t largest = 0, bytes_records = 0;
    const bool read = r.open(path);
    for (const auto& x : r.records())
        if (x.kind == record_kind::bytes) {
            largest = std::max(largest, x.payload.size());
            ++bytes_records;
        }
    check(read && !r.torn() && largest <= ours().max_frame && bytes_records > (2u << 20) / ours().max_frame,
          "journaled as records of at most max_frame bytes");
    file_journal j2;
    j2.f.open_append(path, r.valid_end());
    wire w2;
    ours_t again(ours(), j2, w2, p);
    check(again.recover(r) && again.state().next_expected() == us.state().next_expected() &&
              again.orders().exec_ids() == us.orders().exec_ids() && w2.delivered == w.delivered,
          "replayed: the same numbers, the same reports, the same ExecIDs in the book");
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
    backlog(d);
    std::filesystem::remove_all(d);
    return test::result();
}
