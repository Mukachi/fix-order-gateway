// The Order State Change Matrices in scope (note 007, D2; FIX 4.4 Volume 4),
// each as a script of its rows, main path. Our side is the gateway's journaled
// session with its order book; the counterparty a FIX session over in-memory
// pipes. Two ways:
//  - scripted: the venue's rows sent as the matrix gives them (crossings
//    included), and after each our order's state checked against the row;
//  - simulated: the matrices a book produces run against the venue simulator,
//    its reports checked against the same rows through our order book.
// Each line printed starts with the matrix ("B.1.a: ..."); `run.py
// conformance` lists them. Spec: docs/design/007-order-entry.md
#include "fix_text.hpp"
#include "venue.hpp"

#include <fixgw/recovery.hpp>
#include <fixgw/venue_sim.hpp>

#include <functional>
#include <string>
#include <vector>

namespace {

using namespace fixgw;
using fixgw::test::check;

// One row. out: we send `type` (D, F, G) with ClOrdID `id`, OrigClOrdID
// `orig`, quantity `qty`; `refused` if the gateway must refuse it. in: the
// venue sends an ExecutionReport (8) or OrderCancelReject (9); the row's
// OrdStatus, OrderQty, CumQty and LeavesQty are what our order must show after.
struct row {
    bool out = false;
    std::string type, id, orig;
    char exec_type = 0, status = 0;
    long order_qty = 0, cum = 0, leaves = 0, last = 0;
    bool refused = false;
    std::string extra;  // more fields for an in row ('|' for SOH)
};
row send(std::string type, std::string id, std::string orig, long qty, bool refused = false) {
    row r;
    r.out = true;
    r.type = std::move(type);
    r.id = std::move(id);
    r.orig = std::move(orig);
    r.order_qty = qty;
    r.refused = refused;
    return r;
}
row exec(std::string id, std::string orig, char exec_type, char status, long qty, long cum, long leaves, long last = 0,
         std::string extra = "") {
    return {false, "8", std::move(id), std::move(orig), exec_type, status, qty, cum, leaves, last, false, std::move(extra)};
}
row reject(std::string id, std::string orig, char status, char response_to = '1') {
    row r;
    r.out = false;
    r.type = "9";
    r.id = std::move(id);
    r.orig = std::move(orig);
    r.exec_type = response_to;  // CxlRejResponseTo
    r.status = status;
    return r;
}

// The two sides joined by in-memory pipes.
struct pair {
    struct to_wire {
        std::string* wire;
        order_event last{};
        int events = 0;
        std::vector<order_event> all;
        void write(std::string_view b) { wire->append(b); }
        void deliver(std::string_view, bool) {}
        void order_update(const order_event& e, bool) {
            last = e;
            ++events;
            all.push_back(e);
        }
        void disconnect() {}
    };
    struct venue_side {
        std::string* wire;
        std::function<void(const message_view<>&)> on_app;
        void write(std::string_view b) { wire->append(b); }
        void disconnect() {}
        void deliver(const message_view<>& m) {
            if (on_app) on_app(m);
        }
        void log(session_log, std::string_view, std::uint64_t) {}
        bool resend(const message_view<>&) { return true; }
        bool release(std::string_view, std::string_view) { return true; }
    };

    std::string to_venue, to_us;
    test::memory_journal j;
    test::yes y;
    to_wire out{&to_venue, {}, 0, {}};
    journaled_session<test::memory_journal, to_wire, test::yes> us;
    venue_side vs{&to_us, {}};
    test::venue_store vst;
    session<venue_side, test::venue_store> venue;
    std::int64_t ms = 0;

    static order_config small() {
        order_config c;
        c.max_orders = 256;
        c.max_exec_ids = 4096;
        c.id_bytes = 1 << 16;
        return c;
    }
    pair() : us(test::client(), j, out, y, small()), venue(test::venue(), vs, vst) {
        us.on_connected(now());
        venue.on_connected(now());
        pump();
    }
    session_time now() { return {ms * 1'000'000, 1'792'567'800'000 + ms}; }
    void pump() {
        for (int k = 0; k < 64 && (!to_venue.empty() || !to_us.empty()); ++k) {
            if (!to_venue.empty()) to_venue.erase(0, venue.on_bytes(to_venue.data(), to_venue.size(), now()));
            if (!to_us.empty()) to_us.erase(0, us.on_bytes(to_us.data(), to_us.size(), now()));
        }
    }
};

std::string body_for(const row& r) {
    const std::string q = std::to_string(r.order_qty);
    if (r.type == "D") return "11=" + r.id + "|55=VOD.L|54=1|60=20261021-07:30:00.000|38=" + q + "|40=2|44=50|" + r.extra;
    if (r.type == "F") return "41=" + r.orig + "|11=" + r.id + "|55=VOD.L|54=1|60=20261021-07:30:00.000|38=" + q + "|";
    return "41=" + r.orig + "|11=" + r.id + "|55=VOD.L|54=1|60=20261021-07:30:00.000|38=" + q + "|40=2|44=50|";
}

std::string report_for(const row& r, int exec_id) {
    if (r.type == "9")
        return "37=V1|11=" + r.id + "|41=" + r.orig + "|39=" + std::string(1, r.status) + "|434=" +
               std::string(1, r.exec_type) + "|102=0|";
    std::string b = "37=V1|11=" + r.id + "|";
    if (!r.orig.empty()) b += "41=" + r.orig + "|";
    b += "17=E" + std::to_string(exec_id) + "|150=" + std::string(1, r.exec_type) + "|39=" + std::string(1, r.status) +
         "|55=VOD.L|54=1|38=" + std::to_string(r.order_qty) + "|151=" + std::to_string(r.leaves) + "|14=" +
         std::to_string(r.cum) + "|6=50|";
    if (r.exec_type == 'F' || r.exec_type == 'G') b += "32=" + std::to_string(r.last) + "|31=50|";
    if (r.exec_type == 'G' || r.exec_type == 'H') b += "19=E1|";
    if (r.exec_type == 'D') b += "378=1|";
    return b + r.extra;
}

bool matches(const order_event& e, const row& r, std::string& why) {
    const auto q = [](long v) { return static_cast<fixed>(v) * fixed_one; };
    if (e.ord_status != r.status) why = std::string("OrdStatus ") + e.ord_status + ", the matrix " + r.status;
    else if (r.type == "8" && (e.order_qty != q(r.order_qty) || e.cum_qty != q(r.cum) || e.leaves_qty != q(r.leaves)))
        why = "quantities " + std::to_string(e.order_qty / fixed_one) + "/" + std::to_string(e.cum_qty / fixed_one) + "/" +
              std::to_string(e.leaves_qty / fixed_one) + ", the matrix " + std::to_string(r.order_qty) + "/" +
              std::to_string(r.cum) + "/" + std::to_string(r.leaves);
    else if (e.inconsistent) why = "marked inconsistent: " + std::string(e.why());
    return why.empty();
}

// Scripted: the venue's rows sent as given.
void scripted(const std::string& name, const std::vector<row>& rows) {
    pair p;
    int exec_id = 0, n = 0;
    std::string failure;
    for (const row& r : rows) {
        ++n;
        p.ms += 10;
        if (r.out) {
            const bool refused = p.us.send(r.type, test::soh(body_for(r)), p.now()) == send_result::refused;
            if (refused != r.refused) failure = "row " + std::to_string(n) + ": " + (refused ? "refused: " + std::string(p.us.last_refusal().why ? p.us.last_refusal().why : "") : "not refused");
            p.pump();
        } else {
            const int before = p.out.events;
            p.venue.send(r.type, test::soh(report_for(r, ++exec_id)), p.now());
            p.pump();
            std::string why;
            if (p.out.events != before + 1) why = "no state after the report";
            else matches(p.out.last, r, why);
            if (!why.empty()) failure = "row " + std::to_string(n) + ": " + why;
        }
        if (!failure.empty()) break;
    }
    check(failure.empty(), name + ": scripted, " + std::to_string(rows.size()) + " rows, our order's state as the matrix after each" +
                               (failure.empty() ? "" : " (" + failure + ")"));
}

// Simulated: our requests and the counterparty's orders sent in order; the
// simulator answers; every state our order book reports is compared, in order,
// with the matrix's rows from the venue.
struct step {
    enum { ours, theirs, end_of_day } kind;
    row r;          // ours: the request; theirs: unused
    long qty = 0;   // theirs: a sell of this many at 50, filling ours
};
step ours(row r) { return {step::ours, std::move(r)}; }
step theirs(long qty) { return {step::theirs, {}, qty}; }
step eod() { return {step::end_of_day, {}}; }

void simulated(const std::string& name, const std::vector<step>& steps, const std::vector<row>& expected,
               const std::string& d_extra = "") {
    pair p;
    venue_sim sim;
    std::vector<order_event> states;
    const auto emit = [&](int s, std::string_view t, const std::string& b) {
        if (s == 1) p.venue.send(t, b, p.now());
    };
    p.vs.on_app = [&](const message_view<>& m) { sim.on_message(1, m, emit); };
    int counter = 0;
    std::string failure;
    for (const step& st : steps) {
        p.ms += 10;
        const int before = p.out.events;
        if (st.kind == step::ours) {
            row d = st.r;
            if (d.type == "D") d.extra = d_extra;
            if ((p.us.send(d.type, test::soh(body_for(d)), p.now()) == send_result::refused) != d.refused)
                failure = "request " + d.id + (d.refused ? " not refused" : " refused");
        } else if (st.kind == step::theirs) {
            const std::string m = test::message("35=D|49=OTHER|56=VENUE|34=1|52=20261021-07:30:00.000|11=S" +
                                                std::to_string(++counter) + "|55=VOD.L|54=2|60=20261021-07:30:00.000|38=" +
                                                std::to_string(st.qty) + "|40=2|44=50|");
            message_view<> v;
            decode(m.data(), m.size(), v);
            sim.on_message(2, v, emit);
        } else {
            sim.end_of_day(emit);
        }
        p.pump();
        (void)before;
    }
    // every state reported, in order (the book records one per report)
    std::size_t k = 0;
    for (const order_event& e : p.out.all) {
        if (!failure.empty()) break;
        if (k == expected.size()) {
            failure = "more reports than the matrix has rows";
            break;
        }
        std::string why;
        if (!matches(e, expected[k], why)) failure = "report " + std::to_string(k + 1) + ": " + why;
        ++k;
    }
    if (failure.empty() && k != expected.size()) failure = std::to_string(k) + " reports, the matrix has " + std::to_string(expected.size());
    check(failure.empty(), name + ": simulated, the venue simulator's " + std::to_string(expected.size()) +
                               " reports as the matrix" + (failure.empty() ? "" : " (" + failure + ")"));
}

// The matrices, main path, from FIX 4.4 Volume 4 (X the order, Y and Z its
// replacements or cancels). OrdStatus: 0 New, 1 PartiallyFilled, 2 Filled,
// 3 DoneForDay, 4 Canceled, 6 PendingCancel, 8 Rejected, A PendingNew, E
// PendingReplace. ExecType: 0 New, 3 DoneForDay, 4 Canceled, 5 Replaced,
// 6 PendingCancel, 8 Rejected, D Restated, E PendingReplace, F Trade, G Trade
// Correct, H Trade Cancel, I OrderStatus.
void vanilla() {
    scripted("A.1.a", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 2000, 8000, 2000), exec("X", "", 'F', '1', 10000, 3000, 7000, 1000),
                       exec("X", "", 'F', '2', 10000, 10000, 0, 7000)});
    scripted("A.1.b", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 2000, 8000, 2000), exec("X", "", 'F', '1', 10000, 3000, 7000, 1000),
                       exec("X", "", '3', '3', 10000, 3000, 0)});
    simulated("A.1.a", {ours(send("D", "X", "", 10000)), theirs(2000), theirs(1000), theirs(7000)},
              {exec("X", "", '0', '0', 10000, 0, 10000), exec("X", "", 'F', '1', 10000, 2000, 8000, 2000),
               exec("X", "", 'F', '1', 10000, 3000, 7000, 1000), exec("X", "", 'F', '2', 10000, 10000, 0, 7000)});
    simulated("A.1.b", {ours(send("D", "X", "", 10000)), theirs(2000), theirs(1000), eod()},
              {exec("X", "", '0', '0', 10000, 0, 10000), exec("X", "", 'F', '1', 10000, 2000, 8000, 2000),
               exec("X", "", 'F', '1', 10000, 3000, 7000, 1000), exec("X", "", '3', '3', 10000, 3000, 0)});
}

void cancels() {
    scripted("B.1.a", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000), send("F", "Y", "X", 10000),
                       exec("Y", "X", '6', '6', 10000, 0, 10000), exec("Y", "X", '4', '4', 10000, 0, 0)});
    scripted("B.1.b", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 2000, 8000, 2000), send("F", "Y", "X", 10000),
                       exec("X", "", 'F', '1', 10000, 5000, 5000, 3000), exec("Y", "X", '6', '6', 10000, 5000, 5000),
                       exec("X", "", 'F', '6', 10000, 6000, 4000, 1000), exec("Y", "X", '4', '4', 10000, 6000, 0)});
    scripted("B.1.c", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 2000, 8000, 2000), send("F", "Y", "X", 10000),
                       exec("X", "", 'F', '1', 10000, 5000, 5000, 3000), exec("Y", "X", '6', '6', 10000, 5000, 5000),
                       exec("X", "", 'F', '6', 10000, 10000, 0, 5000), reject("Y", "X", '2')});
    scripted("B.1.d", {send("D", "X", "", 10000), send("F", "Y", "X", 10000), exec("Y", "X", '6', '6', 10000, 0, 10000),
                       exec("X", "", '0', '0', 10000, 0, 10000), exec("Y", "X", '4', '4', 10000, 0, 0),
                       send("D", "A", "", 5000), send("F", "B", "A", 5000), exec("B", "A", '6', '6', 5000, 0, 5000),
                       exec("B", "A", '4', '4', 5000, 0, 0)});
    scripted("B.1.e", {send("D", "X", "", 10000), send("F", "Y", "X", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("Y", "X", '6', '6', 10000, 0, 10000), exec("Y", "X", '4', '4', 10000, 0, 0)});
    // B.1.f, an unknown order: the gateway refuses the request before it leaves.
    scripted("B.1.f", {send("F", "Y", "X", 10000, true)});
    simulated("B.1.a", {ours(send("D", "X", "", 10000)), ours(send("F", "Y", "X", 10000))},
              {exec("X", "", '0', '0', 10000, 0, 10000), exec("Y", "X", '6', '6', 10000, 0, 10000), exec("Y", "X", '4', '4', 10000, 0, 0)});
    simulated("B.1.b", {ours(send("D", "X", "", 10000)), theirs(2000), ours(send("F", "Y", "X", 10000))},
              {exec("X", "", '0', '0', 10000, 0, 10000), exec("X", "", 'F', '1', 10000, 2000, 8000, 2000),
               exec("Y", "X", '6', '6', 10000, 2000, 8000), exec("Y", "X", '4', '4', 10000, 2000, 0)});
}

void replaces() {
    scripted("C.1.a", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000), send("G", "Y", "X", 11000),
                       exec("Y", "X", 'E', 'E', 10000, 0, 10000), exec("Y", "X", '5', '0', 11000, 0, 11000),
                       exec("Y", "", 'F', '1', 11000, 1000, 10000, 1000), exec("Y", "", 'F', '1', 11000, 3000, 8000, 2000)});
    scripted("C.1.b", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 1000, 9000, 1000), send("G", "Y", "X", 12000),
                       exec("Y", "X", 'E', 'E', 10000, 1000, 9000), exec("X", "", 'F', 'E', 10000, 1100, 8900, 100),
                       exec("Y", "X", '5', '1', 12000, 1100, 10900), exec("Y", "", 'F', '2', 12000, 12000, 0, 10900)});
    scripted("C.1.c", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '2', 10000, 10000, 0, 10000), send("G", "Y", "X", 12000),
                       exec("Y", "X", 'E', 'E', 10000, 10000, 0), exec("Y", "X", '5', '1', 12000, 10000, 2000),
                       exec("Y", "", 'F', '2', 12000, 12000, 0, 2000)});
    scripted("C.2.a", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 1000, 9000, 1000), send("G", "Y", "X", 10000),
                       exec("X", "", 'F', '2', 10000, 10000, 0, 9000), reject("Y", "X", '2', '2')});
    scripted("C.3.a", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 1000, 9000, 1000), send("G", "Y", "X", 8000),
                       exec("X", "", 'F', '1', 10000, 1500, 8500, 500), exec("Y", "X", 'E', 'E', 10000, 1500, 8500),
                       exec("X", "", 'F', 'E', 10000, 1600, 8400, 100), exec("Y", "X", '5', '1', 8000, 1600, 6400),
                       exec("Y", "", 'F', '2', 8000, 8000, 0, 6400)});
    scripted("C.3.b", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000), send("G", "Y", "X", 7000),
                       exec("X", "", 'F', '1', 10000, 7000, 3000, 7000), exec("Y", "X", '5', '2', 7000, 7000, 0)});
    scripted("C.3.c", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000), send("G", "Y", "X", 7000),
                       exec("X", "", 'F', '1', 10000, 8000, 2000, 8000), exec("Y", "X", '5', '2', 8000, 8000, 0)});
    simulated("C.1.a", {ours(send("D", "X", "", 10000)), ours(send("G", "Y", "X", 11000)), theirs(1000), theirs(2000)},
              {exec("X", "", '0', '0', 10000, 0, 10000), exec("Y", "X", 'E', 'E', 10000, 0, 10000),
               exec("Y", "X", '5', '0', 11000, 0, 11000), exec("Y", "", 'F', '1', 11000, 1000, 10000, 1000),
               exec("Y", "", 'F', '1', 11000, 3000, 8000, 2000)});
    simulated("C.1.c", {ours(send("D", "X", "", 10000)), theirs(10000), ours(send("G", "Y", "X", 12000)), theirs(2000)},
              {exec("X", "", '0', '0', 10000, 0, 10000), exec("X", "", 'F', '2', 10000, 10000, 0, 10000),
               exec("Y", "X", 'E', 'E', 10000, 10000, 0), exec("Y", "X", '5', '1', 12000, 10000, 2000),
               exec("Y", "", 'F', '2', 12000, 12000, 0, 2000)});
    simulated("C.3.c", {ours(send("D", "X", "", 10000)), theirs(8000), ours(send("G", "Y", "X", 7000))},
              {exec("X", "", '0', '0', 10000, 0, 10000), exec("X", "", 'F', '1', 10000, 8000, 2000, 8000),
               exec("Y", "X", 'E', 'E', 10000, 8000, 2000), exec("Y", "X", '5', '2', 8000, 8000, 0)});
}

void sequencing_and_chaining() {
    const std::vector<row> start = {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                                    exec("X", "", 'F', '1', 10000, 1000, 9000, 1000)};
    const auto with = [&](std::vector<row> rest) {
        std::vector<row> v = start;
        v.insert(v.end(), rest.begin(), rest.end());
        return v;
    };
    scripted("D.1.a", with({send("G", "Y", "X", 8000), exec("Y", "X", 'E', 'E', 10000, 1000, 9000),
                            exec("X", "", 'F', 'E', 10000, 1500, 8500, 500), exec("Y", "X", '5', '1', 8000, 1500, 6500),
                            exec("Y", "", 'F', '1', 8000, 3500, 4500, 2000), send("G", "Z", "Y", 6000),
                            exec("Z", "Y", 'E', 'E', 8000, 3500, 4500), exec("Y", "", 'F', 'E', 8000, 4000, 4000, 500),
                            exec("Z", "Y", '5', '1', 6000, 4000, 2000), exec("Z", "", 'F', '2', 6000, 6000, 0, 2000)}));
    scripted("D.1.b", with({send("G", "Y", "X", 8000), reject("Y", "X", '1', '2'), exec("X", "", 'F', '1', 10000, 1500, 8500, 500),
                            exec("X", "", 'F', '1', 10000, 3500, 6500, 2000), send("G", "Z", "X", 6000),
                            exec("Z", "X", 'E', 'E', 10000, 3500, 6500), exec("Z", "X", '5', '1', 6000, 3500, 2500),
                            exec("Z", "", 'F', '1', 6000, 5000, 1000, 1500)}));
    scripted("D.1.c", with({send("G", "Y", "X", 8000), exec("Y", "X", 'E', 'E', 10000, 1000, 9000),
                            exec("X", "", 'F', 'E', 10000, 1500, 8500, 500), reject("Y", "X", '1', '2'),
                            exec("X", "", 'F', '1', 10000, 3500, 6500, 2000), send("G", "Z", "X", 6000),
                            exec("Z", "X", 'E', 'E', 10000, 3500, 6500), exec("Z", "X", '5', '1', 6000, 3500, 2500),
                            exec("Z", "", 'F', '1', 6000, 5000, 1000, 1500)}));
    scripted("D.2.a", with({send("G", "Y", "X", 8000), send("G", "Z", "Y", 7000), exec("Y", "X", 'E', 'E', 10000, 1000, 9000),
                            exec("Y", "X", '5', '1', 8000, 1000, 7000), exec("Z", "Y", 'E', 'E', 8000, 1000, 7000),
                            exec("Z", "Y", '5', '1', 7000, 1000, 6000), exec("Z", "", 'F', '2', 7000, 7000, 0, 6000)}));
    scripted("D.2.b", with({send("G", "Y", "X", 8000), send("G", "Z", "Y", 7000), exec("Y", "X", 'E', 'E', 10000, 1000, 9000),
                            exec("Z", "X", 'E', 'E', 8000, 1000, 7000), exec("Y", "X", '5', 'E', 8000, 1000, 7000),
                            exec("Z", "Y", '5', '1', 7000, 1000, 6000), exec("Z", "", 'F', '2', 7000, 7000, 0, 6000)}));
    scripted("D.2.c", with({send("G", "Y", "X", 8000), send("G", "Z", "Y", 7000), exec("Y", "X", 'E', 'E', 10000, 1000, 9000),
                            reject("Y", "X", '1', '2'), exec("Z", "X", 'E', 'E', 10000, 1000, 9000), reject("Z", "X", '1', '2'),
                            exec("X", "", 'F', '1', 10000, 7000, 3000, 6000)}));
    scripted("D.2.d", with({send("G", "Y", "X", 8000), send("G", "Z", "Y", 7000), exec("Y", "X", 'E', 'E', 10000, 1000, 9000),
                            reject("Z", "X", 'E', '2'), exec("Y", "X", '5', '1', 8000, 1000, 7000),
                            exec("Y", "", 'F', '1', 8000, 3000, 5000, 2000)}));
}

void unsolicited_rejects_and_time_in_force() {
    scripted("E.1.b", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 1000, 9000, 1000), exec("X", "", '4', '4', 10000, 1000, 0)});
    // F.1.a and F.1.b: the gateway never sends a ClOrdID twice; the venue's
    // answers to one that arrives twice, through the simulator, below.
    scripted("F.1.a", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 1000, 9000, 1000), send("D", "X", "", 15000, true)});
    scripted("F.1.b", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'I', '0', 10000, 0, 10000), send("D", "X", "", 10000, true)});
    scripted("I.1.a", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000), exec("X", "", '4', '4', 10000, 0, 0)});
    scripted("I.1.b", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 1000, 9000, 1000), exec("X", "", '4', '4', 10000, 1000, 0)});
    simulated("I.1.a", {theirs(5000), ours(send("D", "X", "", 10000))},
              {exec("X", "", '0', '0', 10000, 0, 10000), exec("X", "", '4', '4', 10000, 0, 0)}, "59=4|");
    simulated("I.1.b", {theirs(1000), ours(send("D", "X", "", 10000))},
              {exec("X", "", '0', '0', 10000, 0, 10000), exec("X", "", 'F', '1', 10000, 1000, 9000, 1000),
               exec("X", "", '4', '4', 10000, 1000, 0)},
              "59=3|");
}

void corrections() {
    scripted("J.1.a", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 1000, 9000, 1000), exec("X", "", 'F', '2', 10000, 10000, 0, 9000),
                       exec("X", "", 'H', '1', 10000, 9000, 1000), exec("X", "", 'G', '1', 10000, 9000, 1000, 9000),
                       exec("X", "", 'F', '2', 10000, 10000, 0, 1000), exec("X", "", 'G', '2', 10000, 10000, 0, 9000),
                       send("G", "Y", "X", 12000), exec("Y", "X", 'E', 'E', 10000, 10000, 0),
                       exec("Y", "X", '5', '1', 12000, 10000, 2000), exec("Y", "", 'G', '1', 12000, 10500, 1500, 9500)});
    scripted("J.1.b", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 5000, 5000, 5000), send("F", "Y", "X", 10000),
                       exec("Y", "X", '6', '6', 10000, 5000, 5000), exec("Y", "X", '4', '4', 10000, 5000, 0),
                       exec("X", "", 'H', '4', 10000, 0, 0), exec("X", "", 'F', '4', 10000, 4000, 0, 4000)});
    scripted("J.1.c", {send("D", "X", "", 10000, false), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 2000, 8000, 2000), exec("X", "", '3', '3', 10000, 2000, 8000),
                       exec("X", "", 'D', '1', 10000, 2000, 8000), exec("X", "", 'F', '1', 10000, 3000, 7000, 1000),
                       exec("X", "", 'G', '1', 10000, 2500, 7500, 1500), exec("X", "", 'G', '1', 10000, 2000, 8000, 500)});
    scripted("J.1.d", {send("D", "X", "", 10000), exec("X", "", '0', '0', 10000, 0, 10000),
                       exec("X", "", 'F', '1', 10000, 5000, 5000, 5000), exec("X", "", '3', '3', 10000, 5000, 0),
                       exec("X", "", 'G', '3', 10000, 4000, 0, 4000), exec("X", "", 'H', '3', 10000, 0, 0)});
}

void venue_duplicates() {
    // F.1.a and F.1.b at the venue: a ClOrdID it has seen arrives again.
    venue_sim sim;
    std::vector<std::string> out;
    const auto emit = [&](int, std::string_view t, const std::string& b) { out.push_back(std::string(t) + "|" + b); };
    const auto order = [&](const std::string& extra, const std::string& qty) {
        const std::string m = test::message("35=D|49=CLIENT|56=VENUE|34=1|52=20261021-07:30:00.000|" + extra +
                                            "11=X|55=VOD.L|54=1|60=20261021-07:30:00.000|38=" + qty + "|40=2|44=50|");
        message_view<> v;
        decode(m.data(), m.size(), v);
        sim.on_message(1, v, emit);
    };
    const auto has = [&](const std::string& s, const std::string& f) { return s.find(test::soh(f)) != std::string::npos; };
    order("", "10000");
    order("", "15000");
    check(out.size() == 2 && has(out[1], "|150=8|") && has(out[1], "|39=0|") && has(out[1], "|103=6|") && has(out[1], "|38=10000|"),
          "F.1.a: simulated, a ClOrdID again without PossResend: Rejected, OrdRejReason 6, the first order's status and quantity");
    order("97=Y|", "10000");
    check(out.size() == 3 && has(out[2], "|150=I|") && has(out[2], "|39=0|"),
          "F.1.b: simulated, a ClOrdID again with PossResend: an OrderStatus report of the order, not a new order");
}

}  // namespace

int main() {
    vanilla();
    cancels();
    replaces();
    sequencing_and_chaining();
    unsolicited_rejects_and_time_in_force();
    corrections();
    venue_duplicates();
    return test::result();
}
