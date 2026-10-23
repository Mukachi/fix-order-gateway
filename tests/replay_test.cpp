// Note 007, S5: replay rebuilds the order book exactly. Random scripts drive
// our journaled session against the venue simulator (orders, cancels,
// replaces, the counterparty's orders that fill ours, dropped connections),
// the journal written to a file; a new session then recovers from that file,
// and every ClOrdID used, refused ones included, must read the same state.
// Also: the simulator never produces a report the book finds inconsistent.
// Spec: docs/design/007-order-entry.md
#include "fix_text.hpp"
#include "venue.hpp"

#include <fixgw/journal.hpp>
#include <fixgw/recovery.hpp>
#include <fixgw/venue_sim.hpp>

#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace fixgw;
using fixgw::test::check;

// Appends to the file; durable at once (the file is read back by the test,
// not after a crash, so no fdatasync is needed).
struct file_journal {
    journal_file& f;
    bool append(record_kind k, session_time t, std::string_view a, std::string_view b) { return f.append(k, t, a, b); }
    durability make_durable(session_time) { return durability::durable; }
    durability poll(session_time) { return durability::durable; }
};

struct to_wire {
    std::string* wire;
    int inconsistent = 0;
    void write(std::string_view b) { wire->append(b); }
    void deliver(std::string_view, bool) {}
    void order_update(const order_event& e, bool) { inconsistent += e.inconsistent; }
    void disconnect() {}
};
struct venue_side {
    std::string* wire;
    venue_sim* sim;
    std::string* to_us_unused;
    void write(std::string_view b) { wire->append(b); }
    void disconnect() {}
    void deliver(const message_view<>& m);
    void log(session_log, std::string_view, std::uint64_t) {}
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
    session<venue_side, test::venue_store>* self = nullptr;
    std::int64_t* ms = nullptr;
};
session_time at(std::int64_t ms) { return {ms * 1'000'000, 1'792'567'800'000 + ms}; }
void venue_side::deliver(const message_view<>& m) {
    sim->on_message(1, m, [&](int s, std::string_view t, const std::string& b) {
        if (s == 1) self->send(t, b, at(*ms));
    });
}

order_config small() {
    order_config c;
    c.max_orders = 4096;
    c.max_exec_ids = 1 << 15;
    c.id_bytes = 1 << 20;
    return c;
}

bool same(const std::optional<order_event>& a, const std::optional<order_event>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->ord_status == b->ord_status && a->id() == b->id() && a->order_qty == b->order_qty && a->cum_qty == b->cum_qty &&
           a->leaves_qty == b->leaves_qty && a->avg_px == b->avg_px;
}

// One script; true if the replayed book equals the live one.
std::map<char, int> final_states;  // across all scripts: how far the scripts reach

bool one(const std::string& path, unsigned seed, int steps, int& inconsistent, std::string& why) {
    std::mt19937 rng(seed);
    journal_file f;
    if (!f.create(path, 1, 0)) {
        why = "journal not created";
        return false;
    }
    file_journal j{f};
    std::string to_venue, to_us;
    test::yes y;
    to_wire out{&to_venue};
    journaled_session<file_journal, to_wire, test::yes> us(test::client(), j, out, y, small());
    venue_sim sim;
    std::int64_t ms = 0;
    venue_side vs{&to_us, &sim, nullptr};
    test::venue_store vst;
    session<venue_side, test::venue_store> venue(test::venue(), vs, vst);
    vs.self = &venue;
    vs.ms = &ms;
    const auto pump = [&] {
        for (int k = 0; k < 64 && (!to_venue.empty() || !to_us.empty()); ++k) {
            if (!to_venue.empty()) to_venue.erase(0, venue.on_bytes(to_venue.data(), to_venue.size(), at(ms)));
            if (!to_us.empty()) to_us.erase(0, us.on_bytes(to_us.data(), to_us.size(), at(ms)));
        }
    };
    const auto connect = [&] {
        us.on_connected(at(ms));
        venue.on_connected(at(ms));
        pump();
    };
    connect();
    std::vector<std::string> ids;
    int taker = 0;
    for (int i = 0; i < steps; ++i) {
        ms += 1 + static_cast<int>(rng() % 5);
        const unsigned r = rng() % 100;
        const std::string px = std::to_string(48 + rng() % 5), qty = std::to_string(100 * (1 + rng() % 10));
        if (r < 35) {
            const std::string id = "O" + std::to_string(i);
            ids.push_back(id);
            const char* tif = rng() % 4 == 0 ? "59=3|" : rng() % 8 == 0 ? "59=4|" : "";
            us.send("D", test::soh("11=" + id + "|55=VOD.L|54=1|60=20261021-07:30:00.000|38=" + qty + "|40=2|44=" + px + "|" + tif),
                    at(ms));
        } else if (r < 50 && !ids.empty()) {
            const std::string orig = ids[rng() % ids.size()], id = "C" + std::to_string(i);
            ids.push_back(id);
            us.send("F", test::soh("41=" + orig + "|11=" + id + "|55=VOD.L|54=1|60=20261021-07:30:00.000|38=" + qty + "|"), at(ms));
        } else if (r < 65 && !ids.empty()) {
            const std::string orig = ids[rng() % ids.size()], id = "R" + std::to_string(i);
            ids.push_back(id);
            us.send("G", test::soh("41=" + orig + "|11=" + id + "|55=VOD.L|54=1|60=20261021-07:30:00.000|38=" + qty +
                                   "|40=2|44=" + px + "|"),
                    at(ms));
        } else if (r < 95) {  // the counterparty sells into our bids
            const std::string m = test::message("35=D|49=OTHER|56=VENUE|34=1|52=20261021-07:30:00.000|11=S" +
                                                std::to_string(++taker) + "|55=VOD.L|54=2|60=20261021-07:30:00.000|38=" +
                                                qty + "|40=2|44=" + px + "|59=3|");
            message_view<> v;
            decode(m.data(), m.size(), v);
            sim.on_message(2, v, [&](int s, std::string_view t, const std::string& b) {
                if (s == 1) venue.send(t, b, at(ms));
            });
        } else {  // the connection drops; the venue cancels our live orders; we reconnect
            us.on_disconnected(at(ms));
            venue.on_disconnected(at(ms));
            to_venue.clear();
            to_us.clear();
            sim.on_disconnect(1, [&](int s, std::string_view t, const std::string& b) {
                if (s == 1) venue.send(t, b, at(ms));
            });
            connect();
        }
        pump();
    }
    inconsistent += out.inconsistent;

    journal_reader rd;
    if (!rd.open(path)) {
        why = "journal not read back";
        return false;
    }
    std::string sink;
    to_wire out2{&sink};
    test::memory_journal mj;
    journaled_session<test::memory_journal, to_wire, test::yes> again(test::client(), mj, out2, y, small());
    if (!again.recover(rd)) {
        why = "recovery failed";
        return false;
    }
    for (const auto& id : ids) {
        if (const auto st = us.orders().state(id); st && id[0] == 'O') ++final_states[st->ord_status];
        if (!same(us.orders().state(id), again.orders().state(id))) {
            why = "ClOrdID " + id + " differs after replay";
            return false;
        }
    }
    if (us.orders().orders() != again.orders().orders() || us.orders().exec_ids() != again.orders().exec_ids()) {
        why = "order or ExecID counts differ";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    char t[] = "replay_test_XXXXXX";
    const char* d = ::mkdtemp(t);
    if (!d) return 1;
    constexpr int scripts = 200, steps = 300;
    int ok = 0, inconsistent = 0;
    std::string why;
    for (unsigned s = 1; s <= scripts; ++s) {
        if (one(std::string(d) + "/" + std::to_string(s) + ".journal", s, steps, inconsistent, why)) ++ok;
        else break;
    }
    std::string reach;
    for (const auto& [st, n] : final_states) reach += (reach.empty() ? "" : ", ") + std::to_string(n) + " " + st;
    check(ok == scripts, "S5: " + std::to_string(ok) + " of " + std::to_string(scripts) + " random scripts of " +
                             std::to_string(steps) + " steps: every order's state after replay equals the state before" +
                             (why.empty() ? "" : " (" + why + ")") + "; orders' final OrdStatus: " + reach);
    check(inconsistent == 0, "D1: the venue simulator's reports never break the book's invariants (" + std::to_string(inconsistent) +
                                 " marked inconsistent)");
    std::filesystem::remove_all(d);
    return test::result();
}
