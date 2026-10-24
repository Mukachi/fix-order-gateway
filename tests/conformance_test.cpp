// The FIX Session Layer Test Cases (V1.0, June 2020), case by case (note 006):
// each sub-case a script of bytes and clock readings into the session, or for
// 1B a and 1S b the transport and the acceptor gate, and checks on what comes
// out. Each line printed starts with its case ("2q: ..."); `run.py
// conformance` builds CONFORMANCE.md from them. Spec: docs/design/006-conformance.md
#include "fix_text.hpp"

#include "venue.hpp"

#include <fixgw/acceptor_gate.hpp>
#include <fixgw/recovery.hpp>
#include <fixgw/session.hpp>
#include <fixgw/transport.hpp>

#include <map>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace fixgw;
using fixgw::test::check;

struct sink {
    std::vector<std::string> written, delivered, logs;
    int disconnects = 0;
    void write(std::string_view b) { written.emplace_back(b); }
    void disconnect() { ++disconnects; }
    void deliver(const message_view<>& m) { delivered.emplace_back(m.bytes()); }
    void log(session_log, std::string_view what, std::uint64_t) { logs.emplace_back(what); }
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
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

constexpr std::int64_t base_ms = 1'792'567'800'000;  // 2026-10-21 07:30:00 UTC
constexpr const char* base_ts = "20261021-07:30:00.000";
constexpr std::int64_t sec = 1'000'000'000;

session_time at(std::int64_t seconds) { return {seconds * sec, base_ms + seconds * 1000}; }

std::string field(const std::string& m, int tag) {
    const std::string key = "\x01" + std::to_string(tag) + "=";
    const auto at_ = m.find(key);
    if (at_ == std::string::npos) return "";
    const auto start = at_ + key.size();
    return m.substr(start, m.find('\x01', start) - start);
}
std::string type(const std::string& m) { return field(m, 35); }

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
    session<sink, store> s;
    explicit fixture(const session_config& c) : s(c, k, st) {}
    std::size_t feed(const std::string& m, std::int64_t t = 0) { return s.on_bytes(m.data(), m.size(), at(t)); }
    std::string last() const { return k.written.empty() ? "" : k.written.back(); }
};

// An initiator logged on: our Logon is 1, theirs 1; next out 2, next expected 2.
struct logged_on : fixture {
    explicit logged_on(const session_config& c = initiator_config()) : fixture(c) {
        s.on_connected(at(0));
        feed(c.role == session_role::initiator ? venue("A", 1, "98=0|108=30|") : client("A", 1, "98=0|108=30|"));
    }
};

// An acceptor logged on: their Logon 1, ours 1.
session_config acceptor_logged_on() { return acceptor_config(); }

// A Reject for message `seq` with `reason` and RefTagID `tag` was the last
// thing sent, and the number was consumed.
bool rejected(const logged_on& f, std::uint64_t seq, int reason, int tag, std::uint64_t next_expected) {
    const std::string m = f.last();
    return type(m) == "3" && field(m, 45) == std::to_string(seq) && field(m, 373) == std::to_string(reason) &&
           (tag == 0 || field(m, 371) == std::to_string(tag)) && f.s.next_expected() == next_expected &&
           f.k.delivered.empty() && f.k.disconnects == 0;
}

// A message after one that was ignored as garbled: processed with the same
// number, so the number was not consumed.
bool next_processed(logged_on& f, std::uint64_t seq) {
    f.feed(venue("D", seq, test::order_fields("N")));
    return f.k.delivered.size() == 1 && f.s.next_expected() == seq + 1;
}

void scenario_1b() {
    {
        tcp_listener l;
        const bool listening = l.open(0);
        const int fd = connect_start(l.port());
        connect_status st = connect_status::in_progress;
        int accepted = -1;
        for (int k = 0; k < 1'000'000 && (st == connect_status::in_progress || accepted < 0); ++k) {
            if (st == connect_status::in_progress) st = connect_check(fd);
            if (accepted < 0) accepted = l.accept();
        }
        check(listening && fd >= 0 && st == connect_status::connected && accepted >= 0,
              "1B a: transport connection established (TCP over loopback, non-blocking connect)");
        if (fd >= 0) ::close(fd);
        if (accepted >= 0) ::close(accepted);
    }
    {
        fixture f(initiator_config());
        f.s.on_connected(at(0));
        check(f.k.written.size() == 1 && type(f.last()) == "A" && field(f.last(), 34) == "1" && field(f.last(), 98) == "0",
              "1B b: Logon sent, MsgSeqNum 1, EncryptMethod 0");
        f.feed(venue("A", 1, "98=0|108=30|"));
        check(f.s.state() == session_state::active && f.k.written.size() == 1, "1B c: valid Logon reply: logged on, nothing sent");
    }
    {
        fixture f(initiator_config());
        f.s.on_connected(at(0));
        f.feed(venue("A", 5, "98=0|108=30|"));
        check(type(f.last()) == "2" && field(f.last(), 7) == "1", "1B c: Logon reply with MsgSeqNum too high: ResendRequest");
    }
    {
        fixture f(initiator_config());
        f.s.on_connected(at(0));
        f.feed(venue("A", 1, "98=0|108=30|55=VOD.L|"));
        check(type(f.last()) == "5" && !field(f.last(), 58).empty() && f.k.disconnects == 1,
              "1B d: invalid Logon reply (a tag not defined for Logon): Logout with the reason, disconnect");
    }
    {
        fixture f(initiator_config());
        f.s.on_connected(at(0));
        f.feed(venue("0", 1));
        check(type(f.last()) == "5" && f.k.disconnects == 1, "1B e: first message not a Logon: Logout, disconnect");
    }
}

void scenario_1s_2s() {
    {
        fixture f(acceptor_config());
        f.s.on_connected(at(0));
        f.feed(client("A", 1, "98=0|108=20|"));
        check(f.s.state() == session_state::active && type(f.last()) == "A", "1S a: valid Logon: Logon in reply");
    }
    {
        fixture f(acceptor_config());
        f.s.on_connected(at(0));
        f.feed(client("A", 4, "98=0|108=20|"));
        check(f.k.written.size() == 2 && type(f.k.written[0]) == "A" && type(f.k.written[1]) == "2",
              "1S a: Logon with MsgSeqNum too high: Logon in reply, then ResendRequest");
    }
    {
        // Two connections log on with the same identity; the gate admits the
        // first and closes the second without writing to it.
        acceptor_gate g;
        g.add("CLIENT", "VENUE", 7);
        int a[2], b[2];
        const bool pairs = ::socketpair(AF_UNIX, SOCK_STREAM, 0, a) == 0 && ::socketpair(AF_UNIX, SOCK_STREAM, 0, b) == 0;
        const std::string logon = client("A", 1, "98=0|108=20|");
        int admitted = 0, refused_dup = 0;
        if (pairs) {
            (void)!::write(a[1], logon.data(), logon.size());
            g.hold(a[0], 0);
            g.poll(0, [&](int, int fd, std::string) { ++admitted; ::close(fd); }, [&](acceptor_gate::refusal) {});
            (void)!::write(b[1], logon.data(), logon.size());
            g.hold(b[0], 0);
            g.poll(0, [&](int, int fd, std::string) { ++admitted; ::close(fd); },
                   [&](acceptor_gate::refusal r) { refused_dup += r == acceptor_gate::refusal::already_connected; });
        }
        char c;
        const bool nothing_sent = pairs && ::recv(b[1], &c, 1, MSG_DONTWAIT) == 0;  // closed, no bytes
        check(pairs && admitted == 1 && refused_dup == 1 && nothing_sent,
              "1S b: Logon with a duplicate identity: disconnected, no message sent (acceptor gate)");
        if (pairs) {
            ::close(a[1]);
            ::close(b[1]);
        }
    }
    {
        fixture f(acceptor_config());
        f.s.on_connected(at(0));
        f.feed(from("STRANGER", "VENUE", "A", 1, "98=0|108=20|"));
        check(f.k.written.empty() && f.k.disconnects == 1, "1S c: Logon from an unconfigured identity: disconnected, nothing sent");
    }
    {
        fixture f(acceptor_config());
        f.s.on_connected(at(0));
        f.feed(client("A", 1, "98=0|"));
        check(type(f.last()) == "5" && !field(f.last(), 58).empty() && f.k.disconnects == 1,
              "1S d: invalid Logon (HeartBtInt missing): Logout with the reason, disconnect");
    }
    {
        fixture f(acceptor_config());
        f.s.on_connected(at(0));
        f.feed(client("D", 1, test::order_fields("X")));
        check(f.k.written.empty() && f.k.disconnects == 1, "2S: first message not a Logon: disconnected, nothing sent");
    }
}

void scenario_2() {
    {
        logged_on f;
        f.feed(venue("D", 2, test::order_fields("A")));
        check(f.k.delivered.size() == 1 && f.s.next_expected() == 3, "2a: MsgSeqNum as expected: accepted");
        check(f.k.delivered.size() == 1, "2h: BeginString FIX.4.4 as expected: accepted");
        check(f.k.delivered.size() == 1, "2j: SenderCompID and TargetCompID as expected: accepted");
        check(f.k.delivered.size() == 1, "2l: BodyLength correct: accepted");
        check(f.k.delivered.size() == 1, "2p: MsgType valid (NewOrderSingle): accepted");
        check(f.k.delivered.size() == 1, "2s: BeginString, BodyLength, MsgType the first three fields: accepted");
        check(f.k.delivered.size() == 1, "3a: CheckSum valid: accepted");
        check(f.k.delivered.size() == 1, "3d: CheckSum last, three characters, delimited by SOH: accepted");
    }
    {
        logged_on f;
        f.feed(venue("D", 4, test::order_fields("A")));
        check(type(f.last()) == "2" && field(f.last(), 7) == "2" && f.k.delivered.empty(),
              "2b: MsgSeqNum higher than expected: ResendRequest");
    }
    {
        logged_on f;
        f.feed(venue("0", 2));
        f.feed(venue("0", 1));
        check(type(f.last()) == "5" && field(f.last(), 58).starts_with("MsgSeqNum too low, expecting 3") && f.k.disconnects == 1,
              "2c: MsgSeqNum lower than expected without PossDupFlag: Logout with the reason, disconnect");
    }
    {
        logged_on f;
        const std::size_t sent = f.k.written.size();
        f.feed("garbage\x01" "8=FIX.4.4\x01" "9=x\x01");
        f.feed(test::message("35=0|49=VENUE|56=CLIENT|34=2|52=20261021-07:30:00.000|", 0, 1));
        check(f.k.written.size() == sent && next_processed(f, 2), "2d: garbled message: ignored, number not consumed");
    }
    {
        logged_on g;
        g.feed(venue("D", 2, test::order_fields("A")));
        g.feed(venue("D", 2, "43=Y|122=20261021-07:29:59.000|" + test::order_fields("A")));
        check(g.k.delivered.size() == 1 && g.k.disconnects == 0 && g.s.next_expected() == 3,
              "2e: PossDupFlag Y, OrigSendingTime before SendingTime, MsgSeqNum lower than expected (already received): ignored");
    }
    {
        logged_on f;
        f.feed(venue("D", 2, "43=Y|122=20261021-07:30:05.000|" + test::order_fields("A")));
        check(rejected(f, 2, 10, 122, 3), "2f: PossDupFlag Y, OrigSendingTime after SendingTime: Reject 10, number consumed");
    }
    {
        logged_on f;
        f.feed(venue("D", 2, "43=Y|" + test::order_fields("A")));
        check(rejected(f, 2, 1, 122, 3), "2g: PossDupFlag Y, OrigSendingTime missing: Reject 1, number consumed");
    }
    {
        logged_on f;
        f.feed("8=FIX.4.2\x01" "9=5\x01" "35=0\x01" "10=000\x01");
        check(type(f.last()) == "5" && f.k.disconnects == 1, "2i: BeginString not FIX.4.4: Logout, disconnect");
    }
    {
        logged_on f;
        f.feed(from("OTHER", "CLIENT", "0", 2));
        check(f.k.written.size() >= 2 && type(f.k.written[f.k.written.size() - 2]) == "3" &&
                  field(f.k.written[f.k.written.size() - 2], 373) == "9" && type(f.last()) == "5" && f.k.disconnects == 1,
              "2k: SenderCompID not as expected: Reject 9, Logout, disconnect");
    }
    {
        logged_on f;
        f.feed(test::message("35=0|49=VENUE|56=CLIENT|34=2|52=20261021-07:30:00.000|", 3));
        check(next_processed(f, 2), "2m: BodyLength incorrect: ignored as garbled, number not consumed");
    }
    {
        logged_on f;
        f.feed(venue("0", 2, "", "20261021-07:28:01.000"));  // 119 s behind
        check(f.s.next_expected() == 3 && f.k.disconnects == 0, "2n: SendingTime within the tolerance (119 s of 120): accepted");
    }
    {
        logged_on f;
        f.feed(venue("0", 2, "", "20261021-07:27:00.000"));
        check(f.k.written.size() >= 2 && field(f.k.written[f.k.written.size() - 2], 373) == "10" && type(f.last()) == "5" &&
                  f.k.disconnects == 1,
              "2o: SendingTime 3 minutes out: Reject 10, Logout, disconnect");
    }
    {
        logged_on f;
        f.feed(venue("ZZ", 2));
        check(rejected(f, 2, 11, 35, 3), "2q: MsgType not valid (ZZ): Reject 11, number consumed");
    }
    {
        logged_on f;
        f.feed(venue("R", 2, "131=Q1|146=1|55=VOD.L|"));
        const std::string m = f.last();
        check(type(m) == "j" && field(m, 45) == "2" && field(m, 372) == "R" && field(m, 380) == "3" &&
                  f.s.next_expected() == 3 && f.k.delivered.empty(),
              "2r: MsgType valid but not supported (QuoteRequest): BusinessMessageReject 3, number consumed");
        const std::size_t before = f.k.written.size();
        f.feed(venue("2", 3, "7=2|16=0|"));
        check(f.k.written.size() == before + 1 && type(f.last()) == "j" && field(f.last(), 43) == "Y",
              "2r: the BusinessMessageReject is resent when asked for, not gap-filled");
    }
    {
        logged_on f;
        std::string m = test::message("35=0|49=VENUE|56=CLIENT|34=2|52=20261021-07:30:00.000|");
        const std::size_t nine = m.find("\x01" "9="), end9 = m.find('\x01', nine + 1);
        const std::string body_length = m.substr(nine + 1, end9 - nine);  // "9=..\x01"
        m.erase(nine + 1, body_length.size());
        m.insert(m.find("\x01" "49=") + 1, body_length);  // 8, 35, 9, ...
        f.feed(m);
        check(next_processed(f, 2), "2t: BeginString, BodyLength, MsgType not the first three: ignored as garbled, number not consumed");
    }
}

void scenario_3() {
    {
        logged_on f;
        f.feed(test::message("35=0|49=VENUE|56=CLIENT|34=2|52=20261021-07:30:00.000|", 0, 7));
        check(next_processed(f, 2), "3b: CheckSum invalid: ignored as garbled, number not consumed");
    }
    {
        logged_on f;
        f.feed("\x01\x01" "35=0\x01" "zz");
        check(f.k.disconnects == 0 && next_processed(f, 2), "3c: garbled bytes: ignored, number not consumed");
    }
    {
        logged_on f;
        std::string m = test::message("35=0|49=VENUE|56=CLIENT|34=2|52=20261021-07:30:00.000|");
        m.erase(m.size() - 2, 1);  // CheckSum of two characters
        f.feed(m);
        check(next_processed(f, 2), "3e: CheckSum not three characters: ignored as garbled, number not consumed");
    }
}

void scenarios_4_to_8() {
    {
        logged_on f;
        f.s.on_tick(at(30));
        check(type(f.last()) == "0", "4a: nothing sent for HeartBtInt: Heartbeat");
    }
    {
        logged_on f;
        f.feed(venue("1", 2, "112=T1|"));
        check(type(f.last()) == "0" && field(f.last(), 112) == "T1", "4b: TestRequest: Heartbeat with its TestReqID");
    }
    {
        logged_on f;
        const std::size_t sent = f.k.written.size();
        f.feed(venue("0", 2));
        check(f.s.next_expected() == 3 && f.k.written.size() == sent, "5: valid Heartbeat: accepted");
    }
    {
        logged_on f;
        f.s.on_tick(at(35));
        const bool none_yet = f.k.written.size() == 2 && type(f.last()) == "0";  // the 30 s Heartbeat only
        f.s.on_tick(at(36));
        const std::string id = field(f.last(), 112);
        f.feed(venue("0", 2, "112=" + id + "|"), 37);
        f.s.on_tick(at(80));
        check(none_yet && type(f.k.written[2]) == "1" && !id.empty() && f.k.disconnects == 0,
              "6: nothing received for HeartBtInt plus 20%: TestRequest; its Heartbeat received");
    }
    {
        logged_on f;
        f.feed(venue("3", 2, "45=1|373=99|"));
        check(f.s.next_expected() == 3 && f.k.disconnects == 0, "7: valid Reject: number consumed, carry on");
    }
    {
        logged_on f;
        f.s.send("D", test::soh(test::order_fields("O1")), at(1));
        f.feed(venue("2", 2, "7=1|16=0|"));
        const auto n = f.k.written.size();
        check(n >= 4 && type(f.k.written[n - 2]) == "4" && field(f.k.written[n - 2], 123) == "Y" &&
                  type(f.k.written[n - 1]) == "D" && field(f.k.written[n - 1], 43) == "Y",
              "8: ResendRequest: Logon gap-filled, the order resent with PossDupFlag Y");
    }
}

void scenario_9_optional() {
    session_config c = initiator_config();
    c.reset_on_logon = true;
    fixture f(c);
    f.s.on_connected(at(0));
    check(field(f.last(), 141) == "Y" && field(f.last(), 34) == "1" && f.st.resets == 1,
          "9: both sides reset to 1 and start a new session (ResetSeqNumFlag at Logon, note 003 D4)");
}

void scenarios_10_11() {
    {
        logged_on f;
        f.feed(venue("4", 5, "123=Y|36=9|"));
        check(type(f.last()) == "2" && field(f.last(), 7) == "2", "10a: GapFill, MsgSeqNum above expected: ResendRequest");
    }
    {
        logged_on f;
        f.feed(venue("4", 2, "123=Y|36=10|"));
        check(f.s.next_expected() == 10, "10b: GapFill, MsgSeqNum as expected: next expected is NewSeqNo");
    }
    {
        logged_on f;
        f.feed(venue("0", 2));
        f.feed(venue("4", 1, "43=Y|122=20261021-07:29:59.000|123=Y|36=5|"));
        check(f.s.next_expected() == 3 && f.k.disconnects == 0, "10c: GapFill, MsgSeqNum below expected, PossDupFlag Y: ignored");
    }
    {
        logged_on f;
        f.feed(venue("0", 2));
        f.feed(venue("4", 1, "123=Y|36=5|"));
        check(type(f.last()) == "5" && field(f.last(), 58).starts_with("MsgSeqNum too low") && f.k.disconnects == 1,
              "10d: GapFill, MsgSeqNum below expected, no PossDupFlag: Logout, disconnect");
    }
    {
        logged_on f;
        f.feed(venue("4", 2, "123=Y|36=2|"));
        check(rejected(f, 2, 5, 36, 3), "10e: GapFill, NewSeqNo not above MsgSeqNum: Reject 5");
    }
    {
        logged_on f;
        f.feed(venue("4", 7, "36=20|"));
        check(f.s.next_expected() == 20, "11a: Reset, NewSeqNo above expected, any MsgSeqNum: next expected is NewSeqNo");
    }
    {
        logged_on f;
        f.feed(venue("4", 7, "36=2|"));
        bool warned = false;
        for (const auto& l : f.k.logs) warned |= l.find("SequenceReset to the expected") != std::string::npos;
        check(f.s.next_expected() == 2 && warned, "11b: Reset, NewSeqNo equal to expected: accepted, warning");
    }
    {
        logged_on f;
        f.feed(venue("0", 2));
        f.feed(venue("4", 9, "36=2|"));
        check(type(f.last()) == "3" && field(f.last(), 373) == "5" && f.s.next_expected() == 3,
              "11c: Reset, NewSeqNo below expected: Reject 5, next expected unchanged");
    }
}

void scenarios_12_13() {
    {
        logged_on f;
        f.s.logout("", at(1));
        const bool sent = type(f.last()) == "5";
        f.s.on_tick(at(11));
        check(sent && f.k.disconnects == 1, "12: Logout sent; no reply in 10 s: disconnect");
    }
    {
        logged_on f;
        f.s.logout("", at(1));
        const std::size_t n = f.k.written.size();
        f.feed(venue("5", 2), 2);
        check(f.k.written.size() == n && f.k.disconnects == 1, "13a: Logout in reply to ours: disconnect, nothing sent");
    }
    {
        logged_on f;
        f.feed(venue("5", 2), 1);
        const bool replied = type(f.last()) == "5" && f.k.disconnects == 0;
        f.s.on_tick(at(11));
        check(replied && f.k.disconnects == 1, "13b: unsolicited Logout: Logout in reply; no disconnect in 10 s: disconnect");
    }
}

void scenario_14() {
    {
        logged_on f;
        f.feed(venue("0", 2, "1000=x|"));
        check(rejected(f, 2, 0, 1000, 3), "14a: tag not defined in FIX 4.4 (1000): Reject 0, number consumed");
    }
    {
        logged_on f;
        f.feed(venue("0", 2, "20=0|"));
        check(rejected(f, 2, 0, 20, 3), "14a: tag 20 (ExecTransType, not in FIX 4.4): Reject 0, number consumed");
    }
    {
        logged_on f;
        f.feed(venue("0", 2, "5001=x|"));
        check(rejected(f, 2, 2, 5001, 3), "14a: a user-defined tag (5001) is defined: on Heartbeat it is Reject 2, not 0");
    }
    {
        logged_on f;
        f.feed(venue("1", 2));
        check(rejected(f, 2, 1, 112, 3), "14b: required tag missing (TestRequest without TestReqID): Reject 1");
    }
    {
        logged_on f;
        f.feed(venue("3", 2, "373=99|"));
        check(rejected(f, 2, 1, 45, 3), "14b: required tag missing (Reject without RefSeqNum): Reject 1");
    }
    {
        logged_on f;
        f.feed(venue("0", 2, "55=VOD.L|"));
        check(rejected(f, 2, 2, 55, 3), "14c: tag defined but not for this message type (Symbol on Heartbeat): Reject 2");
    }
    {
        logged_on f;
        f.feed(venue("0", 2, "112=|"));
        check(rejected(f, 2, 4, 112, 3), "14d: tag without a value: Reject 4");
    }
    {
        logged_on f;
        f.feed(venue("4", 2, "123=X|36=5|"));
        check(rejected(f, 2, 5, 123, 3), "14e: value not one of the enumerated values (GapFillFlag X): Reject 5");
    }
    {
        logged_on f(acceptor_logged_on());
        f.feed(client("D", 2, test::order_fields("U1") + "847=1001|"));
        f.feed(client("D", 3, test::order_fields("U2") + "847=999|"));
        check(f.k.delivered.size() == 1 && type(f.last()) == "3" && field(f.last(), 45) == "3" && field(f.last(), 373) == "5" &&
                  field(f.last(), 371) == "847",
              "14e: TargetStrategy 1001 (its union datatype admits 1000 and up, EP311): accepted; 999: Reject 5");
    }
    {
        logged_on f;
        f.feed(venue("2", 2, "7=abc|16=0|"));
        check(rejected(f, 2, 6, 7, 3), "14f: value in the wrong format (BeginSeqNo abc): Reject 6");
    }
    {
        logged_on f;
        f.feed(venue("0", 2, "112=a|50=SUB|"));
        check(rejected(f, 2, 14, 50, 3), "14g: header tag after the body: Reject 14");
    }
    {
        logged_on f;
        f.feed(venue("0", 2, "112=a|112=b|"));
        check(rejected(f, 2, 13, 112, 3), "14h: tag not in a group appears twice: Reject 13");
    }
    {
        logged_on f;
        f.feed(venue("0", 2, "627=2|628=H1|"));
        check(rejected(f, 2, 16, 627, 3), "14i: NumInGroup 2 with one entry (NoHops): Reject 16");
    }
    {
        logged_on f;
        f.feed(venue("0", 2, "627=1|628=H1|628=H2|"));
        check(rejected(f, 2, 16, 627, 3), "14i: NumInGroup 1 with two entries (NoHops): Reject 16");
    }
    {
        logged_on f;
        f.feed(venue("0", 2, "627=1|629=20261021-07:30:00|628=H1|"));
        check(rejected(f, 2, 15, 629, 3), "14j: group fields out of order (HopSendingTime before HopCompID): Reject 15");
    }
    {
        logged_on f;
        f.feed(venue("5", 2, "58=ab|cd|"));
        check(rejected(f, 2, 17, 58, 3), "14k: a non-data value contains SOH: Reject 17");
    }
    {
        logged_on f;
        f.feed(venue("0", 2, "627=1|628=H1|629=20261021-07:30:00|630=4|"));
        check(f.s.next_expected() == 3 && f.k.written.size() == 1, "14i: a whole NoHops group: accepted");
    }
}

void scenario_14m() {
    {
        logged_on f(acceptor_logged_on());
        f.feed(client("D", 2, "11=L1|55=VOD.L|54=1|60=20261021-07:30:00.000|38=100|40=2|"));
        const std::string m = f.last();
        check(type(m) == "j" && field(m, 45) == "2" && field(m, 372) == "D" && field(m, 380) == "5" &&
                  field(m, 58).ends_with("44") && f.s.next_expected() == 3 && f.k.delivered.empty(),
              "14m: a limit NewOrderSingle without Price: BusinessMessageReject 5 naming tag 44, number consumed");
    }
    {
        logged_on f;
        f.feed(venue("8", 2, "37=X1|11=O1|17=E1|150=F|39=2|55=VOD.L|54=1|31=72.5|151=0|14=100|6=72.5|"));
        check(type(f.last()) == "j" && field(f.last(), 380) == "5" && field(f.last(), 58).ends_with("32") && f.k.delivered.empty(),
              "14m: a fill (ExecType Trade) without LastQty: BusinessMessageReject 5 naming tag 32");
    }
}

void scenario_19a() {
    // The order book (note 007, D3) behind the session: a report whose ExecID
    // was seen is not delivered again, PossResend or not.
    struct out_t {
        std::vector<std::string> delivered;
        void write(std::string_view) {}
        void deliver(std::string_view m, bool) { delivered.emplace_back(m); }
        void disconnect() {}
    } out;
    test::memory_journal j;
    test::yes y;
    order_config oc;
    oc.max_orders = 64;
    oc.max_exec_ids = 256;
    oc.id_bytes = 4096;
    journaled_session<test::memory_journal, out_t, test::yes> us(initiator_config(), j, out, y, oc);
    const auto feed = [&](const std::string& m) { us.on_bytes(m.data(), m.size(), at(0)); };
    us.on_connected(at(0));
    feed(venue("A", 1, "98=0|108=30|"));
    us.send("D", test::soh(test::order_fields("O1")), at(0));
    feed(venue("8", 2, test::report_fields("O1", "E1")));
    feed(venue("8", 3, "97=Y|" + test::report_fields("O1", "E1")));
    check(out.delivered.size() == 1 && us.state().next_expected() == 4,
          "19a: PossResend Y, the report already seen (its ExecID): not delivered again, its number consumed");
}

void scenario_15_optional() {
    logged_on f;
    f.feed(test::message("35=0|52=20261021-07:30:00.000|34=2|56=CLIENT|49=VENUE|"));
    f.feed(test::message("35=0|34=3|49=VENUE|52=20261021-07:30:00.000|56=CLIENT|"));
    check(f.s.next_expected() == 4 && f.k.written.size() == 1,
          "15: header fields in a different order, twice: accepted, the numbers after them accepted");
}

void scenarios_16_19_20() {
    {
        fixture f(initiator_config());
        const auto r = f.s.send("D", test::soh(test::order_fields("Q1")), at(0));
        f.s.on_connected(at(5));
        f.feed(venue("A", 1, "98=0|108=30|"), 5);
        check(r == send_result::queued, "16a: an order while disconnected: queued, unnumbered");
        check(f.k.written.size() == 2 && type(f.k.written[1]) == "D" && field(f.k.written[1], 34) == "2" &&
                  field(f.k.written[1], 52) == "20261021-07:30:05.000",
              "16b: after logon: sent with the next number and the time of sending");
    }
    {
        logged_on f;
        f.feed(venue("D", 2, "97=Y|" + test::order_fields("A")));
        check(f.k.delivered.size() == 1, "19b: PossResend Y, not seen before: accepted and delivered");
    }
    {
        logged_on f;
        f.feed(venue("0", 5));  // a gap: ResendRequest 2 to 0 outstanding
        f.feed(venue("2", 6, "7=1|16=0|"));
        const auto n = f.k.written.size();
        check(n >= 3 && type(f.k.written[n - 1]) == "4", "20: their ResendRequest while ours is outstanding: served");
    }
}

}  // namespace

int main() {
    scenario_1b();
    scenario_1s_2s();
    scenario_2();
    scenario_3();
    scenarios_4_to_8();
    scenario_9_optional();
    scenarios_10_11();
    scenarios_12_13();
    scenario_14();
    scenario_14m();
    scenario_19a();
    scenario_15_optional();
    scenarios_16_19_20();
    return test::result();
}
