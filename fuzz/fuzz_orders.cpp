// Order-entry fuzzer (note 007, S6): our journaled session with its order book
// against a venue session and the venue simulator, over in-memory pipes,
// driven by a script read from the fuzz input: orders, cancels and replaces
// with fuzzed fields (valid or not, ClOrdIDs reused), the counterparty's
// orders that fill ours, dropped connections (what was in flight lost),
// clock steps, and, if the input asks, forged ExecutionReports from the venue.
// Invariants, any breach aborting:
//   with no forged report, the simulator's reports never break the book's
//   invariants (D1); at the end, reconnected and quiet, every new order the
//   gateway accepted has the venue's state in our book, or was reported not
//   sent and is unknown to the venue; no crash, no sanitizer report.
#include <fixgw/encode.hpp>
#include <fixgw/recovery.hpp>
#include <fixgw/venue_sim.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

namespace {

using namespace fixgw;

struct mem_journal {
    bool append(record_kind, session_time, std::string_view, std::string_view) { return true; }
    durability make_durable(session_time) { return durability::durable; }
    durability poll(session_time) { return durability::durable; }
};
struct policy {
    void log(session_log, std::string_view, std::uint64_t) {}
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};
struct to_wire {
    std::string* wire;
    int inconsistent = 0;
    std::set<std::string> not_sent;
    void write(std::string_view b) { wire->append(b); }
    void deliver(std::string_view, bool) {}
    void order_update(const order_event& e, bool) {
        inconsistent += e.inconsistent;
        if (e.kind == order_event::not_sent) not_sent.insert(std::string(e.id()));
    }
    void disconnect() {}
};
struct venue_store {
    std::map<std::uint64_t, std::string> m;
    void record(std::uint64_t s, std::string_view b) { m[s] = std::string(b); }
    std::string_view lookup(std::uint64_t s) {
        const auto it = m.find(s);
        return it == m.end() ? std::string_view{} : std::string_view(it->second);
    }
    void reset() { m.clear(); }
};
struct world;
struct venue_side {
    world* w;
    void write(std::string_view b);
    void disconnect() {}
    void deliver(const message_view<>& m);
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
session_config theirs() {
    session_config c;
    c.role = session_role::acceptor;
    c.sender_comp_id = "VENUE";
    c.target_comp_id = "CLIENT";
    return c;
}
order_config small() {
    order_config c;
    c.max_orders = 128;
    c.max_exec_ids = 2048;
    c.id_bytes = 1 << 16;
    c.throttle_messages = 8;
    c.throttle_window_ns = 50'000'000;
    c.stale_ns = 2'000'000'000;
    return c;
}

struct world {
    std::string to_venue, to_us;
    mem_journal j;
    policy p;
    to_wire out{&to_venue, 0, {}};
    journaled_session<mem_journal, to_wire, policy> us{ours(), j, out, p, small()};
    venue_sim sim;
    venue_side vs{this};
    venue_store vst;
    session<venue_side, venue_store> venue{theirs(), vs, vst};
    std::int64_t ms = 0;
    session_time now() const { return {ms * 1'000'000, 1'792'567'800'000 + ms}; }
    void emit(int s, std::string_view t, const std::string& b) {
        if (s == 1) venue.send(t, b, now());
    }
    void pump() {
        for (int k = 0; k < 256 && (!to_venue.empty() || !to_us.empty()); ++k) {
            if (!to_venue.empty()) to_venue.erase(0, venue.on_bytes(to_venue.data(), to_venue.size(), now()));
            if (!to_us.empty()) to_us.erase(0, us.on_bytes(to_us.data(), to_us.size(), now()));
        }
    }
    void connect() {
        us.on_connected(now());
        venue.on_connected(now());
        pump();
    }
    void drop() {
        us.on_disconnected(now());
        venue.on_disconnected(now());
        to_venue.clear();
        to_us.clear();
        sim.on_disconnect(1, [&](int s, std::string_view t, const std::string& b) { emit(s, t, b); });
    }
};
void venue_side::write(std::string_view b) { w->to_us.append(b); }
void venue_side::deliver(const message_view<>& m) {
    w->sim.on_message(1, m, [&](int s, std::string_view t, const std::string& b) { w->emit(s, t, b); });
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    world w;
    w.connect();
    std::size_t i = 0;
    const auto next = [&]() -> std::uint8_t { return i < size ? data[i++] : 0; };
    bool forged = false;
    std::set<std::string> ids;
    int taker = 0;
    while (i < size) {
        const std::uint8_t op = next(), a = next(), b = next();
        const std::string id = std::string(1, "ODCR"[a % 4]) + std::to_string(b % 24);  // few, so they collide
        const std::string qty = std::to_string(100 * (1 + a % 6)), px = std::to_string(48 + b % 5);
        switch (op % 10) {
            case 0:
            case 1: {
                const char* tif = a & 16 ? "59=3|" : a & 32 ? "59=4|" : "";
                std::string body = "11=" + id + "|55=VOD.L|54=1|60=20261021-07:30:00.000|38=" + qty + "|40=" + (a & 64 ? "1" : "2") +
                                   "|" + (a & 64 ? "" : "44=" + px + "|") + tif;
                if (b & 128) body = "11=" + id + "|55=VOD.L|";  // missing required fields: refused by the venue's checks
                for (auto& c : body)
                    if (c == '|') c = '\x01';
                if (w.us.send("D", body, w.now()) != send_result::refused) ids.insert(id);  // a new order of ours
                break;
            }
            case 2:
            case 3: {
                const std::string orig = std::string(1, "ODCR"[b % 4]) + std::to_string(a % 24);
                std::string body = op % 10 == 2
                                       ? "41=" + orig + "|11=" + id + "|55=VOD.L|54=1|60=20261021-07:30:00.000|38=" + qty + "|"
                                       : "41=" + orig + "|11=" + id + "|55=VOD.L|54=1|60=20261021-07:30:00.000|38=" + qty +
                                             "|40=2|44=" + px + "|";
                for (auto& c : body)
                    if (c == '|') c = '\x01';
                w.us.send(op % 10 == 2 ? "F" : "G", body, w.now());
                break;
            }
            case 4:
            case 5: {  // the counterparty sells into our bids
                const std::string body = "35=D\x01" "49=OTHER\x01" "56=VENUE\x01" "34=1\x01" "52=20261021-07:30:00.000\x01" "11=S" +
                                         std::to_string(++taker) + "\x01" "55=VOD.L\x01" "54=2\x01" "60=20261021-07:30:00.000\x01" "38=" +
                                         qty + "\x01" "40=2\x01" "44=" + px + "\x01" "59=3\x01";
                std::string framed = "8=FIX.4.4\x01" "9=" + std::to_string(body.size()) + "\x01" + body;
                unsigned sum = 0;
                for (const unsigned char c : framed) sum += c;
                char t[16];
                std::snprintf(t, sizeof(t), "10=%03u\x01", sum % 256);
                framed += t;
                message_view<> v;
                if (decode(framed.data(), framed.size(), v).ok)
                    w.sim.on_message(2, v, [&](int s, std::string_view tt, const std::string& bb) { w.emit(s, tt, bb); });
                break;
            }
            case 6: w.ms += a; break;
            case 7:
                if (a % 4 == 0) {
                    w.drop();
                    w.ms += b * 4;
                    w.connect();
                }
                break;
            case 8:
                if (a % 8 == 0) {  // a forged report: any ExecType, OrdStatus and quantities for one of ours
                    forged = true;
                    const char et = "0F45E6DGH8I"[b % 11], st = "0124568AE"[a % 9];
                    const std::string body = "37=X\x01" "11=" + id + "\x01" "17=FX" + std::to_string(i) + "\x01" "150=" +
                                             std::string(1, et) + "\x01" "39=" + std::string(1, st) + "\x01" "55=VOD.L\x01" "54=1\x01" "38=" +
                                             qty + "\x01" "151=" + std::to_string(b % 700) + "\x01" "14=" + std::to_string(a % 700) +
                                             "\x01" "6=50\x01" "41=" + id + "\x01" "32=1\x01" "31=50\x01" "19=E1\x01" "378=1\x01";
                    w.venue.send("8", body, w.now());
                }
                break;
            default: break;
        }
        w.ms += 1;
        w.pump();
        if (!forged && w.out.inconsistent > 0) {
            std::fprintf(stderr, "the simulator's reports broke the book's invariants\n");
            std::abort();
        }
    }
    if (forged) return 0;  // forged reports make the book's state the forger's: no comparison
    // Reconnected and quiet: every order as the venue has it.
    w.drop();
    w.ms += 10;
    w.connect();
    for (int k = 0; k < 8; ++k) {
        w.ms += 100;
        w.us.on_tick(w.now());
        w.venue.on_tick(w.now());
        w.pump();
    }
    if (w.us.state().state() != session_state::active || w.venue.state() != session_state::active) return 0;  // e.g. logged out by a check
    for (const auto& id : ids) {
        const auto ours_ = w.us.orders().state(id);
        const auto venue_ = w.sim.state(1, id);
        if (!ours_ || w.out.not_sent.count(id)) {
            if (venue_ && !ours_) {
                std::fprintf(stderr, "%s: the venue has it, our book does not\n", id.c_str());
                std::abort();
            }
            continue;
        }
        if (!venue_) {  // the venue never got it: only if it was reported not sent (handled above)
            {
                std::fprintf(stderr, "%s: our book has it (%c), the venue does not\n", id.c_str(), ours_->ord_status);
                std::abort();
            }
            continue;
        }
        if (venue_->status != ours_->ord_status || venue_->cum != ours_->cum_qty || venue_->leaves != ours_->leaves_qty) {
            std::fprintf(stderr, "%s: ours %c %lld/%lld, the venue's %c %lld/%lld\n", id.c_str(), ours_->ord_status,
                         static_cast<long long>(ours_->cum_qty), static_cast<long long>(ours_->leaves_qty), venue_->status,
                         static_cast<long long>(venue_->cum), static_cast<long long>(venue_->leaves));
            std::abort();
        }
    }
    return 0;
}
