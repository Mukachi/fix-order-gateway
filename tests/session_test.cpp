// Tests for include/fixgw/session.hpp. Spec: docs/design/003-session.md (S1,
// S2, S3, S5): each mandatory Session Layer Test Case scenario listed in S2,
// scripted as bytes and clock readings in, bytes and actions checked.
#include "fix_text.hpp"

#include <fixgw/session.hpp>

#include <map>
#include <string>
#include <vector>

namespace {

using namespace fixgw;
using fixgw::test::check;

struct sink {
    std::vector<std::string> written, delivered, logs;
    int disconnects = 0;
    bool resend_answer = true, release_answer = true;
    void write(std::string_view b) { written.emplace_back(b); }
    void disconnect() { ++disconnects; }
    void deliver(const message_view<>& m) { delivered.emplace_back(m.bytes()); }
    void log(session_log, std::string_view what, std::uint64_t) { logs.emplace_back(what); }
    bool resend(const message_view<>&) { return resend_answer; }
    bool release(std::string_view, std::string_view) { return release_answer; }
};

struct store {
    std::map<std::uint64_t, std::string> sent;
    int resets = 0;
    void record(std::uint64_t seq, std::string_view b) { sent[seq] = std::string(b); }
    std::string_view lookup(std::uint64_t seq) {
        const auto it = sent.find(seq);
        return it == sent.end() ? std::string_view{} : std::string_view(it->second);
    }
    void reset() {
        sent.clear();
        ++resets;
    }
};

using test_session = session<sink, store>;

constexpr std::int64_t base_ms = 1'792'567'800'000;  // 2026-10-21 07:30:00 UTC
constexpr const char* base_ts = "20261021-07:30:00.000";
constexpr std::int64_t sec = 1'000'000'000;

session_time at(std::int64_t seconds) { return {seconds * sec, base_ms + seconds * 1000}; }

// A field of a written message, or "" (first occurrence).
std::string field(const std::string& m, int tag) {
    const std::string key = "\x01" + std::to_string(tag) + "=";
    const auto at_ = m.find(key);
    if (at_ == std::string::npos) return "";
    const auto start = at_ + key.size();
    return m.substr(start, m.find('\x01', start) - start);
}
std::string type(const std::string& m) { return field(m, 35); }

// A message from the counterparty (VENUE to CLIENT, or CLIENT to VENUE for the
// acceptor tests): type, number, then extra body fields ('|' for SOH).
std::string from(const std::string& sender, const std::string& target, const std::string& msg_type, std::uint64_t seq,
                 const std::string& rest = "", const std::string& ts = base_ts) {
    return test::message("35=" + msg_type + "|49=" + sender + "|56=" + target + "|34=" + std::to_string(seq) +
                         "|52=" + ts + "|" + rest);
}
std::string venue(const std::string& t, std::uint64_t seq, const std::string& rest = "", const std::string& ts = base_ts) {
    return from("VENUE", "CLIENT", t, seq, rest, ts);
}
std::string client(const std::string& t, std::uint64_t seq, const std::string& rest = "") {
    return from("CLIENT", "VENUE", t, seq, rest);
}

session_config initiator_config() {
    session_config c;
    c.role = session_role::initiator;
    c.sender_comp_id = "CLIENT";
    c.target_comp_id = "VENUE";
    c.heartbeat_s = 30;
    return c;
}
session_config acceptor_config() {
    session_config c;
    c.role = session_role::acceptor;
    c.sender_comp_id = "VENUE";
    c.target_comp_id = "CLIENT";
    return c;
}

struct fixture {
    sink k;
    store st;
    test_session s;
    explicit fixture(const session_config& c) : s(c, k, st) {}
    std::size_t feed(const std::string& m, std::int64_t t = 0) { return s.on_bytes(m.data(), m.size(), at(t)); }
    std::string last() const { return k.written.empty() ? "" : k.written.back(); }
};

// An initiator logged on: our Logon is 1, theirs 1; next out 2, next expected 2.
struct logged_on : fixture {
    logged_on() : fixture(initiator_config()) {
        s.on_connected(at(0));
        feed(venue("A", 1, "98=0|108=30|"));
    }
};

void scenario_1b() {
    {
        fixture f(initiator_config());
        f.s.on_connected(at(0));
        check(f.k.written.size() == 1 && type(f.last()) == "A" && field(f.last(), 34) == "1" &&
                  field(f.last(), 98) == "0" && field(f.last(), 108) == "30" && field(f.last(), 141).empty(),
              "1B b: initiator sends Logon, MsgSeqNum 1, EncryptMethod 0, HeartBtInt 30");
        f.feed(venue("A", 1, "98=0|108=30|"));
        check(f.s.state() == session_state::active && f.s.next_expected() == 2 && f.k.written.size() == 1,
              "1B c: valid Logon reply: active, nothing sent");
    }
    {
        fixture f(initiator_config());
        f.s.on_connected(at(0));
        f.feed(venue("A", 5, "98=0|108=30|"));
        check(f.s.state() == session_state::resending && type(f.last()) == "2" && field(f.last(), 7) == "1" &&
                  field(f.last(), 16) == "0",
              "1B c: Logon reply with MsgSeqNum too high: ResendRequest 1 to 0");
    }
    {
        fixture f(initiator_config());
        f.s.on_connected(at(0));
        f.feed(venue("A", 1, "98=1|108=30|"));
        check(type(f.last()) == "5" && f.k.disconnects == 1, "1B d: invalid Logon (EncryptMethod 1): Logout, disconnect");
    }
    {
        fixture f(initiator_config());
        f.s.on_connected(at(0));
        f.feed(venue("0", 1));
        check(type(f.last()) == "5" && f.k.disconnects == 1 &&
                  field(f.last(), 58) == "first message not a logon",
              "1B e: first message not a Logon: Logout, disconnect");
    }
    {
        fixture f(initiator_config());
        f.s.on_connected(at(0));
        f.s.on_tick(at(9));
        const bool waiting = f.k.disconnects == 0;
        f.s.on_tick(at(10));
        check(waiting && f.k.disconnects == 1, "1B: no Logon reply in 10 s: disconnect");
    }
}

void scenario_1s() {
    {
        fixture f(acceptor_config());
        f.s.on_connected(at(0));
        check(f.k.written.empty() && f.s.state() == session_state::awaiting_logon, "1S: acceptor waits for Logon");
        f.feed(client("A", 1, "98=0|108=20|"));
        check(f.s.state() == session_state::active && type(f.last()) == "A" && field(f.last(), 108) == "20" &&
                  field(f.last(), 34) == "1",
              "1S a: valid Logon: reply Logon with the counterparty's HeartBtInt");
    }
    {
        fixture f(acceptor_config());
        f.s.on_connected(at(0));
        f.feed(client("A", 4, "98=0|108=20|"));
        check(f.k.written.size() == 2 && type(f.k.written[0]) == "A" && type(f.k.written[1]) == "2" &&
                  field(f.k.written[1], 7) == "1",
              "1S a: Logon with MsgSeqNum too high: reply Logon, then ResendRequest");
    }
    {
        fixture f(acceptor_config());
        f.s.on_connected(at(0));
        f.feed(from("STRANGER", "VENUE", "A", 1, "98=0|108=20|"));
        check(f.k.written.empty() && f.k.disconnects == 1, "1S c: unknown CompID: disconnect, nothing sent");
    }
    {
        fixture f(acceptor_config());
        f.s.on_connected(at(0));
        f.feed(client("A", 1, "98=0|"));
        check(type(f.last()) == "5" && f.k.disconnects == 1, "1S d: invalid Logon (no HeartBtInt): Logout, disconnect");
    }
    {
        fixture f(acceptor_config());
        f.s.on_connected(at(0));
        f.feed(client("D", 1, "11=X|"));
        check(f.k.written.empty() && f.k.disconnects == 1, "2S: first message not a Logon: disconnect, nothing sent");
    }
}

void scenario_2() {
    {
        logged_on f;
        f.feed(venue("D", 2, "11=A|"));
        check(f.k.delivered.size() == 1 && f.s.next_expected() == 3, "2a: MsgSeqNum as expected: delivered");
    }
    {
        logged_on f;
        f.feed(venue("D", 4, "11=A|"));
        check(f.k.delivered.empty() && type(f.last()) == "2" && field(f.last(), 7) == "2" && field(f.last(), 16) == "0" &&
                  f.s.state() == session_state::resending,
              "2b: too high: ResendRequest 2 to 0, the message dropped (D1)");
        f.feed(venue("D", 5, "11=B|"));
        check(f.k.written.size() == 2, "2b: a second message above the gap: no second ResendRequest");
        f.feed(venue("4", 2, "43=Y|122=" + std::string(base_ts) + "|123=Y|36=4|"));
        f.feed(venue("D", 4, "43=Y|122=" + std::string(base_ts) + "|11=A|"));
        f.feed(venue("D", 5, "43=Y|122=" + std::string(base_ts) + "|11=B|"));
        check(f.k.delivered.size() == 2 && f.s.next_expected() == 6 && f.s.state() == session_state::active,
              "2b: resend fills the gap: both delivered once, in order, active again");
    }
    {
        logged_on f;
        f.feed(venue("D", 2, "11=A|"));
        f.feed(venue("D", 2, "11=A|"));
        check(type(f.last()) == "5" && f.k.disconnects == 1 &&
                  field(f.last(), 58) == "MsgSeqNum too low, expecting 3 but received 2",
              "2c: too low without PossDup: Logout with the reason, disconnect");
    }
    {
        logged_on f;
        f.feed(venue("D", 2, "11=A|"));
        const auto n = f.k.written.size();
        f.feed(venue("D", 2, "43=Y|122=" + std::string(base_ts) + "|11=A|"));
        check(f.k.written.size() == n && f.k.delivered.size() == 1 && f.s.next_expected() == 3,
              "2e: too low with PossDup: ignored");
    }
    {
        logged_on f;
        f.feed(venue("D", 2, "43=Y|122=20261021-07:31:00.000|11=A|"));
        check(type(f.last()) == "3" && field(f.last(), 373) == "10" && field(f.last(), 371) == "122" &&
                  f.s.next_expected() == 3 && f.k.delivered.empty(),
              "2f: OrigSendingTime after SendingTime: Reject 10, number consumed");
    }
    {
        logged_on f;
        f.feed(venue("D", 2, "43=Y|11=A|"));
        check(type(f.last()) == "3" && field(f.last(), 373) == "1" && field(f.last(), 371) == "122" &&
                  f.s.next_expected() == 3,
              "2g: PossDup without OrigSendingTime: Reject 1, number consumed");
    }
    {
        logged_on f;
        const std::string m = "8=FIX.4.2\x01" "9=5\x01" "35=0\x01" "10=000\x01";
        f.feed(m);
        check(type(f.last()) == "5" && f.k.disconnects == 1, "2i: BeginString not FIX.4.4: Logout, disconnect");
    }
    {
        logged_on f;
        f.feed(from("OTHER", "CLIENT", "D", 2, "11=A|"));
        check(f.k.written.size() >= 3 && type(f.k.written[f.k.written.size() - 2]) == "3" &&
                  field(f.k.written[f.k.written.size() - 2], 373) == "9" && type(f.last()) == "5" && f.k.disconnects == 1,
              "2k: CompID mismatch: Reject 9, Logout, disconnect");
    }
    {
        logged_on f;
        f.feed(venue("D", 2, "11=A|", "20261021-07:33:00.001"));
        check(type(f.k.written[f.k.written.size() - 2]) == "3" && field(f.k.written[f.k.written.size() - 2], 373) == "10" &&
                  type(f.last()) == "5" && f.k.disconnects == 1,
              "2o: SendingTime 3 minutes out: Reject 10, Logout, disconnect");
    }
    {
        logged_on f;
        f.feed(test::message("35=D|49=VENUE|56=CLIENT|34=2|52=" + std::string(base_ts) + "|11=A|", 0, 1));
        f.feed(test::message("35=D|49=VENUE|56=CLIENT|34=2|52=" + std::string(base_ts) + "|11=A|", 3));
        check(f.k.delivered.empty() && f.s.next_expected() == 2 && f.k.written.size() == 1,
              "2d, 2m, 3b: garbled (CheckSum, BodyLength): ignored, number not consumed");
        f.feed(venue("D", 2, "11=A|"));
        check(f.k.delivered.size() == 1, "2d: the next valid message is processed");
    }
}

void scenario_4_6() {
    {
        logged_on f;
        f.s.on_tick(at(29));
        const auto n = f.k.written.size();
        f.s.on_tick(at(30));
        check(n == 1 && f.k.written.size() == 2 && type(f.last()) == "0", "4a: nothing sent for HeartBtInt: Heartbeat");
    }
    {
        logged_on f;
        f.feed(venue("1", 2, "112=PING-7|"));
        check(type(f.last()) == "0" && field(f.last(), 112) == "PING-7", "4b: TestRequest: Heartbeat with its TestReqID");
    }
    {
        logged_on f;
        f.s.on_tick(at(35));
        const bool not_yet = type(f.last()) == "0";
        f.s.on_tick(at(36));
        check(not_yet && type(f.last()) == "1" && !field(f.last(), 112).empty(),
              "6: nothing received for HeartBtInt plus 20% (36 s): TestRequest, not before");
        const std::string id = field(f.last(), 112);
        f.feed(venue("0", 2, "112=" + id + "|"), 40);
        f.s.on_tick(at(73));
        check(f.k.disconnects == 0, "6: Heartbeat in reply: still connected");
        f.s.on_tick(at(76));
        f.s.on_tick(at(112));
        check(f.k.disconnects == 1, "6: no reply to a second TestRequest within HeartBtInt plus 20%: disconnect");
    }
}

void scenario_7_8() {
    {
        logged_on f;
        f.feed(venue("3", 2, "45=1|373=99|"));
        check(f.s.next_expected() == 3 && f.k.disconnects == 0, "7: Reject received: number consumed, carry on");
    }
    {
        logged_on f;  // we sent: 1 Logon
        f.s.send("D", test::soh("11=A|55=VOD.L|"), at(1));  // 2
        f.s.on_tick(at(31));                                  // 3 Heartbeat
        f.s.send("D", test::soh("11=B|55=VOD.L|"), at(32));  // 4
        const auto n = f.k.written.size();
        f.feed(venue("2", 2, "7=1|16=0|"), 33);
        std::vector<std::string> r(f.k.written.begin() + static_cast<std::ptrdiff_t>(n), f.k.written.end());
        const bool shape = r.size() == 4 && type(r[0]) == "4" && field(r[0], 34) == "1" && field(r[0], 36) == "2" &&
                           field(r[0], 123) == "Y" && field(r[0], 43) == "Y" && type(r[1]) == "D" &&
                           field(r[1], 34) == "2" && field(r[1], 43) == "Y" && field(r[1], 11) == "A" &&
                           field(r[1], 122) == "20261021-07:30:01.000" && field(r[1], 52) == "20261021-07:30:33.000" &&
                           type(r[2]) == "4" && field(r[2], 34) == "3" && field(r[2], 36) == "4" && type(r[3]) == "D" &&
                           field(r[3], 34) == "4" && field(r[3], 11) == "B";
        check(shape, "8: ResendRequest 1 to 0: Logon and Heartbeat gap-filled, orders resent with PossDup and OrigSendingTime (S5)");
        check(f.s.next_outbound() == 5, "8: resending does not advance the outbound number");
        f.k.resend_answer = false;
        const auto m = f.k.written.size();
        f.feed(venue("2", 3, "7=2|16=4|"), 34);
        check(f.k.written.size() == m + 1 && type(f.last()) == "4" && field(f.last(), 34) == "2" &&
                  field(f.last(), 36) == "5",
              "8: the stale-order hook says no: one gap fill 2 to 5");
    }
}

void scenario_10_11() {
    {
        logged_on f;
        f.feed(venue("4", 2, "123=Y|36=7|"));
        check(f.s.next_expected() == 7, "10b: GapFill as expected: expected becomes NewSeqNo");
    }
    {
        logged_on f;
        f.feed(venue("4", 4, "123=Y|36=7|"));
        check(type(f.last()) == "2" && field(f.last(), 7) == "2", "10a: GapFill above expected: ResendRequest");
    }
    {
        logged_on f;
        f.feed(venue("D", 2, "11=A|"));
        f.feed(venue("4", 2, "43=Y|122=" + std::string(base_ts) + "|123=Y|36=3|"));
        check(f.k.disconnects == 0 && f.s.next_expected() == 3, "10c: GapFill too low with PossDup: ignored");
        f.feed(venue("4", 2, "123=Y|36=3|"));
        check(type(f.last()) == "5" && f.k.disconnects == 1, "10d: GapFill too low without PossDup: Logout, disconnect");
    }
    {
        logged_on f;
        f.feed(venue("4", 2, "123=Y|36=2|"));
        check(type(f.last()) == "3" && field(f.last(), 373) == "5" && field(f.last(), 371) == "36",
              "10e: GapFill with NewSeqNo not above MsgSeqNum: Reject 5");
    }
    {
        logged_on f;
        f.feed(venue("4", 99, "36=10|"));
        check(f.s.next_expected() == 10, "11a: Reset above expected, any MsgSeqNum: expected becomes NewSeqNo");
        f.feed(venue("4", 1, "36=10|"));
        check(f.s.next_expected() == 10 && f.k.logs.back() == "SequenceReset to the expected MsgSeqNum",
              "11b: Reset equal to expected: accepted, warning logged");
        f.feed(venue("4", 5, "36=4|"));
        check(type(f.last()) == "3" && field(f.last(), 373) == "5" && f.s.next_expected() == 10,
              "11c: Reset below expected: Reject 5, expected unchanged");
        const auto n = f.k.written.size();
        f.feed(venue("4", 5, "36=9|"));
        check(f.k.written.size() == n + 1 && field(f.last(), 373) == "5" && f.s.next_expected() == 10,
              "11c: Reset one below expected: Reject 5, expected unchanged");
    }
}

void scenario_12_13() {
    {
        logged_on f;
        f.s.logout("end of day", at(1));
        check(type(f.last()) == "5" && field(f.last(), 58) == "end of day" &&
                  f.s.state() == session_state::logout_sent,
              "12: logout(): Logout sent");
        f.feed(venue("5", 2), 2);
        check(f.k.disconnects == 1, "13a: Logout in reply to ours: disconnect, nothing sent");
    }
    {
        logged_on f;
        f.s.logout("", at(1));
        f.s.on_tick(at(10));
        const bool waiting = f.k.disconnects == 0;
        f.s.on_tick(at(11));
        check(waiting && f.k.disconnects == 1, "12: no Logout in reply within 10 s: disconnect");
    }
    {
        logged_on f;
        f.feed(venue("5", 2), 1);
        check(type(f.last()) == "5" && f.s.state() == session_state::logout_replied && f.k.disconnects == 0,
              "13b: unsolicited Logout: Logout sent in reply, wait for the counterparty to disconnect");
        f.s.on_tick(at(11));
        check(f.k.disconnects == 1, "13b: no disconnect within 10 s: disconnect");
    }
}

void scenario_14_16_19_20() {
    {
        logged_on f;
        f.feed(venue("D", 2, "11=|"));
        check(type(f.last()) == "3" && field(f.last(), 373) == "4" && field(f.last(), 371) == "11" &&
                  f.s.next_expected() == 3 && f.k.delivered.empty(),
              "14d: tag without a value: Reject 4, number consumed");
    }
    {
        fixture f(initiator_config());
        const auto r1 = f.s.send("D", test::soh("11=Q1|"), at(0));
        f.s.on_connected(at(1));
        const auto r2 = f.s.send("D", test::soh("11=Q2|"), at(1));
        check(r1 == send_result::queued && r2 == send_result::queued && f.k.written.size() == 1,
              "16a: application messages queued, unnumbered, while not logged on");
        f.feed(venue("A", 1, "98=0|108=30|"), 2);
        check(f.k.written.size() == 3 && field(f.k.written[1], 11) == "Q1" && field(f.k.written[1], 34) == "2" &&
                  field(f.k.written[2], 11) == "Q2" && field(f.k.written[2], 34) == "3" &&
                  field(f.k.written[1], 52) == "20261021-07:30:02.000",
              "16b: after logon, sent in order with new numbers and the time of sending");
    }
    {
        fixture f(initiator_config());
        f.s.send("D", test::soh("11=Q1|"), at(0));
        f.s.on_connected(at(1));
        f.feed(venue("A", 3, "98=0|108=30|"), 2);
        check(f.k.written.size() == 2 && f.s.queued() == 1, "16b: queued messages wait while the gap is recovered");
        f.feed(venue("4", 1, "43=Y|122=" + std::string(base_ts) + "|123=Y|36=4|"), 3);
        check(f.k.written.size() == 3 && field(f.last(), 11) == "Q1" && f.s.queued() == 0,
              "16b: and go out once it is filled");
    }
    {
        fixture f(initiator_config());
        f.k.release_answer = false;
        f.s.send("D", test::soh("11=STALE|"), at(0));
        f.s.on_connected(at(1));
        f.feed(venue("A", 1, "98=0|108=30|"), 2);
        check(f.k.written.size() == 1 && f.s.queued() == 0, "16: the stale-order hook drops a queued order");
    }
    {
        session_config c = initiator_config();
        c.queue_messages = 2;
        fixture f(c);
        f.s.send("D", test::soh("11=1|"), at(0));
        f.s.send("D", test::soh("11=2|"), at(0));
        check(f.s.send("D", test::soh("11=3|"), at(0)) == send_result::refused, "16: queue full: refused");
    }
    {
        logged_on f;
        f.feed(venue("D", 2, "97=Y|11=A|"));
        check(f.k.delivered.size() == 1, "19: PossResend in sequence: delivered (duplicate detection is note 007's)");
    }
    {
        logged_on f;
        f.feed(venue("D", 3, "11=B|"));  // gap: our ResendRequest outstanding
        const auto n = f.k.written.size();
        f.feed(venue("2", 4, "7=1|16=0|"));
        check(f.k.written.size() == n + 1 && type(f.last()) == "4" && field(f.last(), 34) == "1" &&
                  f.s.state() == session_state::resending,
              "20: their ResendRequest while ours is outstanding: served (Logon gap-filled)");
    }
}

void reset_d4() {
    session_config c = initiator_config();
    c.reset_on_logon = true;
    fixture f(c);
    f.s.on_connected(at(0));
    check(field(f.last(), 141) == "Y" && field(f.last(), 34) == "1" && f.st.resets == 1,
          "D4: reset_on_logon: Logon with ResetSeqNumFlag, MsgSeqNum 1, store reset");
    fixture a(acceptor_config());
    a.s.on_connected(at(0));
    a.feed(client("A", 1, "98=0|108=30|141=Y|"));
    check(field(a.last(), 141) == "Y" && field(a.last(), 34) == "1" && a.st.resets == 1 && a.s.next_expected() == 2,
          "D4: acceptor honours ResetSeqNumFlag: replies with it, both restart at 1");
}

// S3: the same inputs to a new session give the same outputs, byte for byte.
void determinism() {
    const auto script = [] {
        logged_on f;
        f.feed(venue("D", 2, "11=A|"), 1);
        f.s.send("D", test::soh("11=O1|"), at(2));
        f.feed(venue("D", 5, "11=C|"), 3);
        f.feed(venue("2", 3, "7=1|16=0|"), 4);
        f.s.on_tick(at(40));
        f.feed(venue("4", 3, "43=Y|122=" + std::string(base_ts) + "|123=Y|36=6|"), 41);
        f.s.logout("bye", at(42));
        return f.k.written;
    };
    check(script() == script(), "S3: same inputs, same outputs byte for byte");
}

}  // namespace

int main() {
    scenario_1b();
    scenario_1s();
    scenario_2();
    scenario_4_6();
    scenario_7_8();
    scenario_10_11();
    scenario_12_13();
    scenario_14_16_19_20();
    reset_d4();
    determinism();
    return test::result();
}
