// Tests for include/fixgw/orders.hpp, the order state machine on its own
// (note 007: D1 states and invariants, D3 duplicates, D4 throttle, D5 stale
// orders). Spec: docs/design/007-order-entry.md
#include "fix_text.hpp"

#include <fixgw/decode.hpp>
#include <fixgw/orders.hpp>

#include <string>

namespace {

using namespace fixgw;
using fixgw::test::check;

session_time at_ms(std::int64_t ms) { return {ms * 1'000'000, 1'792'567'800'000 + ms}; }

// An inbound message decoded, kept alive with its bytes.
struct inbound {
    std::string bytes;
    message_view<> v;
    explicit inbound(const std::string& body) : bytes(test::message(body)) { decode(bytes.data(), bytes.size(), v); }
};

std::string report(const std::string& id, const std::string& exec_id, char exec_type, char status, int cum, int leaves,
                   const std::string& extra = "", int qty = 100) {
    return "35=8|49=VENUE|56=CLIENT|34=9|52=20261021-07:30:00.000|37=X|11=" + id + "|17=" + exec_id + "|150=" +
           std::string(1, exec_type) + "|39=" + std::string(1, status) + "|55=VOD.L|54=1|38=" + std::to_string(qty) + "|151=" + std::to_string(leaves) +
           "|14=" + std::to_string(cum) + "|6=72.5|" + extra;
}

bool send(order_book& b, const std::string& type, const std::string& fields, std::int64_t ms = 0) {
    const std::string body = test::soh(fields);
    const auto v = b.check(type, body, at_ms(ms));
    if (v.ok) b.record(type, body, at_ms(ms));
    return v.ok;
}

char status(const order_book& b, const std::string& id) { return b.state(id) ? b.state(id)->ord_status : '-'; }

void lifecycle() {
    order_book b({1024, 4096, 1 << 16});
    check(send(b, "D", test::order_fields("O1")) && status(b, "O1") == ord_status::pending_new, "D1: an order sent is PendingNew");
    order_event e;
    inbound ack(report("O1", "E1", '0', '0', 0, 100));
    check(b.inbound(ack.v, e) && e.ord_status == ord_status::new_ && !e.inconsistent, "D1: ExecType New: New");
    inbound part(report("O1", "E2", 'F', '1', 40, 60, "32=40|31=72.5|"));
    check(b.inbound(part.v, e) && e.ord_status == ord_status::partially_filled && e.cum_qty == 40 * fixed_one &&
              e.leaves_qty == 60 * fixed_one,
          "D1: a part fill: PartiallyFilled, CumQty 40, LeavesQty 60");
    check(!b.inbound(part.v, e), "D3: the same ExecID again: discarded");
    inbound dup(report("O1", "E2", 'F', '1', 40, 60, "43=Y|122=20261021-07:29:59.000|32=40|31=72.5|"));
    check(!b.inbound(dup.v, e), "D3 (19a): a resent report (PossDup) already seen: discarded");
    inbound fresh(report("O1", "E3", 'F', '1', 50, 50, "43=Y|122=20261021-07:29:59.000|32=10|31=72.5|"));
    check(b.inbound(fresh.v, e) && e.cum_qty == 50 * fixed_one, "19b: a resent report not seen before: applied");

    check(send(b, "F", "41=O1|11=C1|55=VOD.L|54=1|60=20261021-07:30:00.000|38=100|"), "D1: a cancel of a live order passes");
    inbound pend(report("C1", "E4", '6', '6', 50, 50, "41=O1|"));
    check(b.inbound(pend.v, e) && e.ord_status == ord_status::pending_cancel, "D1: PendingCancel");
    inbound done(report("C1", "E5", '4', '4', 50, 0, "41=O1|"));
    check(b.inbound(done.v, e) && e.ord_status == ord_status::canceled && status(b, "O1") == ord_status::canceled,
          "D1: Canceled; the chain's first ClOrdID reads the same order");
    check(!send(b, "F", "41=O1|11=C2|55=VOD.L|54=1|60=20261021-07:30:00.000|38=100|"), "D1: a cancel of a finished order: refused");
}

void replace_and_reject() {
    order_book b({1024, 4096, 1 << 16});
    send(b, "D", test::order_fields("O1"));
    order_event e;
    inbound ack(report("O1", "E1", '0', '0', 0, 100));
    b.inbound(ack.v, e);
    check(send(b, "G", "41=O1|11=R1|55=VOD.L|54=1|60=20261021-07:30:00.000|38=200|40=1|"), "D1: a replace of a live order passes");
    inbound rep(report("R1", "E2", '5', '0', 0, 200, "41=O1|", 200));
    check(b.inbound(rep.v, e) && e.id() == "R1" && e.order_qty == 200 * fixed_one && e.leaves_qty == 200 * fixed_one,
          "D1: Replaced: the order now known by the new ClOrdID, OrderQty 200");
    check(send(b, "F", "41=R1|11=C1|55=VOD.L|54=1|60=20261021-07:30:00.000|38=200|"), "D1: a cancel by the new ClOrdID passes");
    inbound rej("35=9|49=VENUE|56=CLIENT|34=9|52=20261021-07:30:00.000|37=X|11=C1|41=R1|39=0|434=1|");
    check(b.inbound(rej.v, e) && e.ord_status == ord_status::new_ && e.exec_type == '9', "D1: an OrderCancelReject: the order stands, New");
    check(!send(b, "D", test::order_fields("O1")), "D1: a ClOrdID used before: refused");
    check(!send(b, "F", "41=NOPE|11=C9|55=VOD.L|54=1|60=20261021-07:30:00.000|38=1|"), "D1: an unknown OrigClOrdID: refused");
}

void invariants() {
    order_book b({1024, 4096, 1 << 16});
    send(b, "D", test::order_fields("O1"));
    order_event e;
    inbound fill(report("O1", "E1", 'F', '1', 60, 40, "32=60|31=72.5|"));
    b.inbound(fill.v, e);
    inbound fell(report("O1", "E2", 'F', '1', 50, 50, "32=10|31=72.5|"));
    check(b.inbound(fell.v, e) && e.inconsistent, "D1: CumQty falling without a bust: delivered, marked inconsistent");
    inbound bust(report("O1", "E3", 'H', '1', 40, 60, "19=E2|"));
    check(b.inbound(bust.v, e) && !e.inconsistent && e.cum_qty == 40 * fixed_one, "D1 (J): a trade bust lowers CumQty: consistent");
    inbound sum(report("O1", "E4", 'F', '1', 50, 40, "32=10|31=72.5|"));
    check(b.inbound(sum.v, e) && e.inconsistent, "D1: OrderQty not CumQty plus LeavesQty while active: marked inconsistent");
    inbound stranger(report("ZZ", "E5", '0', '0', 0, 100));
    check(b.inbound(stranger.v, e) && e.inconsistent && e.why() == "unknown ClOrdID", "D1: a report for an order not ours: marked, reason given");
}

void throttle() {
    order_config c{1024, 4096, 1 << 16};
    c.throttle_messages = 3;
    c.throttle_window_ns = 1'000'000'000;
    order_book b(c);
    bool three = send(b, "D", test::order_fields("O1"), 0) && send(b, "D", test::order_fields("O2"), 100) &&
                 send(b, "D", test::order_fields("O3"), 200);
    const std::string body = test::soh(test::order_fields("O4"));
    const auto v = b.check("D", body, at_ms(300));
    check(three && !v.ok && v.retry_at_ns == 1'000'000'000, "D4: the fourth order in a second: refused, retry at 1.000 s");
    check(send(b, "F", "41=O1|11=C1|55=VOD.L|54=1|60=20261021-07:30:00.000|38=100|", 300),
          "D4: a cancel of a live order over the limit: passes (counted, never refused)");
    check(!send(b, "D", test::order_fields("O4"), 1000) && send(b, "D", test::order_fields("O4"), 1101),
          "D4: the cancel counted: the window moves on from the second send (at 1.100 s)");
}

void stale() {
    order_book b({1024, 4096, 1 << 16});
    send(b, "D", test::order_fields("O1"), 0);
    send(b, "D", test::order_fields("O2"), 2500);
    order_event e{};
    check(b.may_resend("D", "O2", at_ms(3000), e), "D5: an order 0.5 s old: resent");
    check(!b.may_resend("D", "O1", at_ms(3000), e) && e.kind == order_event::not_sent && e.id() == "O1" &&
              status(b, "O1") == ord_status::rejected,
          "D5: an order 3 s old: not resent, the application told it was not sent, Rejected locally");
    send(b, "F", "41=O2|11=C2|55=VOD.L|54=1|60=20261021-07:30:00.000|38=100|", 2600);
    check(b.may_resend("F", "C2", at_ms(60'000), e), "D5: a cancel of a live order, however old: resent");
}

void fixed_point() {
    check(detail::parse_fixed("72.53") == 7'253'000'000 && detail::parse_fixed("-0.5") == -50'000'000 &&
              detail::parse_fixed("100") == 100 * fixed_one && detail::parse_fixed("0.00000001") == 1,
          "fixed: 72.53, -0.5, 100, 1e-8 read exactly");
    check(!detail::parse_fixed("1.000000001") && !detail::parse_fixed("1..2") && !detail::parse_fixed("") && !detail::parse_fixed("x"),
          "fixed: more than 8 decimals, two points, empty, not a number: refused");
}

}  // namespace

int main() {
    lifecycle();
    replace_and_reject();
    invariants();
    throttle();
    stale();
    fixed_point();
    return test::result();
}
