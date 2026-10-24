// Interoperability with QuickFIX v1.16.0 (note 006, D6, S6), both roles, over
// loopback. Our side is the session (note 003) over a tcp_connection, driven
// here; QuickFIX runs as a separate process, a black box.
//  1. Our initiator against QuickFIX's `executor` example (fills every limit
//     order): log on, 2,000 orders, the connection dropped by our side after
//     700 reports with orders in flight, reconnect, recovery by resend, log out.
//  2. Our acceptor (a venue answering each order) against qf_initiator,
//     written to QuickFIX's API: the same, the drop by our side as acceptor.
// Each checks every order answered, in order, and no Reject either way.
//
// Usage: interop_test <executor> <qf_initiator> <FIX44.xml>
#include "../tests/fix_text.hpp"

#include <fixgw/engine.hpp>
#include <fixgw/pipeline.hpp>
#include <fixgw/session.hpp>
#include <fixgw/transport.hpp>

#include <cstdio>
#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

using namespace fixgw;
using fixgw::test::check;

struct counting_sink {
    tcp_connection* c = nullptr;
    std::vector<std::string> orders;  // ClOrdIDs to answer (venue role)
    std::vector<std::string> reports;  // ClOrdIDs of ExecutionReports received (client role)
    int rejects_sent = 0, rejects_received = 0, error_logouts = 0, resend_requests = 0, poss_dups = 0;
    void write(std::string_view b) {
        if (b.find("\x01" "35=2\x01") != std::string_view::npos) ++resend_requests;
        if (b.find("\x01" "35=3\x01") != std::string_view::npos) ++rejects_sent;
        if (b.find("\x01" "35=5\x01") != std::string_view::npos && b.find("\x01" "58=") != std::string_view::npos) ++error_logouts;
        c->write(b);
    }
    void disconnect() { c->close(); }
    void deliver(const message_view<>& m) {
        const std::string id(m.get(11).value_or(""));
        if (m.get(43) == std::string_view("Y")) ++poss_dups;
        if (m.msg_type() == "D") orders.push_back(id);
        if (m.msg_type() == "8") reports.push_back(id);
    }
    void log(session_log, std::string_view what, std::uint64_t) {
        if (what == "Reject received") ++rejects_received;
    }
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};
struct memory_store {
    std::map<std::uint64_t, std::string> m;
    void record(std::uint64_t s, std::string_view b) { m[s] = std::string(b); }
    std::string_view lookup(std::uint64_t s) {
        const auto it = m.find(s);
        return it == m.end() ? std::string_view{} : std::string_view(it->second);
    }
    void reset() { m.clear(); }
};

pid_t spawn(const std::vector<std::string>& args, const std::string& out) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    pid_t pid = -1;
    if (posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), environ) != 0) pid = -1;
    posix_spawn_file_actions_destroy(&fa);
    return pid;
}

std::string settings(const std::string& dir, bool acceptor, std::uint16_t port, const std::string& dictionary) {
    std::string s = "[DEFAULT]\n";
    s += acceptor ? "ConnectionType=acceptor\nSocketAcceptPort=" + std::to_string(port) + "\n"
                  : "ConnectionType=initiator\nSocketConnectHost=127.0.0.1\nSocketConnectPort=" + std::to_string(port) +
                        "\nReconnectInterval=1\nHeartBtInt=30\n";
    s += "FileStorePath=" + dir + "/store\nFileLogPath=" + dir + "/log\nStartTime=00:00:00\nEndTime=00:00:00\n";
    s += "UseDataDictionary=Y\nDataDictionary=" + dictionary + "\nSocketNodelay=Y\n\n[SESSION]\nBeginString=FIX.4.4\n";
    s += acceptor ? "SenderCompID=VENUE\nTargetCompID=CLIENT\n" : "SenderCompID=CLIENT\nTargetCompID=VENUE\n";
    std::ofstream(dir + "/settings.cfg") << s;
    return dir + "/settings.cfg";
}

// Rejects (35=3) and Logouts with a reason in QuickFIX's logs: its message
// logs (SOH) and the executor's screen log ('|' for SOH), every file but the
// store and the settings.
int quickfix_complaints(const std::string& dir) {
    int n = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir)) {
        const std::string path = e.path().string();
        if (!e.is_regular_file() || path.find("/store") != std::string::npos || path.ends_with(".cfg")) continue;
        std::ifstream f(e.path());
        for (std::string line; std::getline(f, line);)
            for (const char d : {'\x01', '|'}) {
                const std::string reject = std::string(1, d) + "35=3" + d, logout = std::string(1, d) + "35=5" + d,
                                  text = std::string(1, d) + "58=";
                n += line.find(reject) != std::string::npos ||
                     (line.find(logout) != std::string::npos && line.find(text) != std::string::npos);
            }
    }
    return n;
}

std::uint16_t free_port() {
    tcp_listener l;
    l.open(0);
    const std::uint16_t p = l.port();
    l.close();
    return p;
}

bool in_order(const std::vector<std::string>& ids, int n, int& distinct) {
    std::set<int> seen;
    int highest = -1;
    bool ordered = true;
    for (const auto& id : ids) {
        const int k = std::atoi(id.c_str() + 1);
        if (!seen.insert(k).second) continue;  // a resent report for one already seen
        ordered &= k > highest;
        highest = k;
    }
    distinct = static_cast<int>(seen.size());
    return ordered && distinct == n;
}

void our_initiator(const std::string& executor, const std::string& dictionary, const std::string& dir) {
    std::filesystem::create_directories(dir);
    const std::uint16_t port = free_port();
    const pid_t qf = spawn({executor, settings(dir, true, port, dictionary)}, dir + "/executor.out");
    tcp_connection c;
    counting_sink k;
    k.c = &c;
    memory_store st;
    session_config cfg;
    cfg.role = session_role::initiator;
    cfg.sender_comp_id = "CLIENT";
    cfg.target_comp_id = "VENUE";
    session<counting_sink, memory_store> s(cfg, k, st);
    const auto connect = [&] {
        for (int attempt = 0; attempt < 200; ++attempt) {
            const int fd = connect_start(port);
            connect_status cs = connect_status::in_progress;
            for (int i = 0; i < 1'000'000 && cs == connect_status::in_progress; ++i) cs = connect_check(fd);
            if (cs == connect_status::connected) {
                c.attach(fd);
                s.on_connected(clock_now());
                return true;
            }
            ::close(fd);
            ::usleep(20'000);  // the executor still starting
        }
        return false;
    };
    constexpr int n = 2000;
    bool ok = connect();
    int submitted = 0, reconnects = 0;
    bool dropped = false, logged_out = false;
    std::string inbox;
    const std::int64_t give_up = clock_now().mono_ns + 60'000'000'000;
    while (ok && clock_now().mono_ns < give_up) {
        char b[16384];
        long r;
        while ((r = c.read_some(b, sizeof(b))) > 0) inbox.append(b, static_cast<std::size_t>(r));
        if (!inbox.empty()) inbox.erase(0, s.on_bytes(inbox.data(), inbox.size(), clock_now()));
        if (r < 0 || !c.open()) {
            s.on_disconnected(clock_now());
            if (logged_out) break;
            inbox.clear();
            ok = connect();
            ++reconnects;
            continue;
        }
        s.on_tick(clock_now());
        std::set<std::string> distinct(k.reports.begin(), k.reports.end());
        const int answered = static_cast<int>(distinct.size());
        if (!dropped && answered >= 700 && s.state() == session_state::active) {
            // Drop with orders in flight: 20 more sent, then closed unread, so
            // their reports are missed and must come back by resend.
            for (int i = 0; i < 20 && submitted < n; ++i, ++submitted)
                s.send("D", test::soh("11=O" + std::to_string(submitted) + "|21=1|55=VOD.L|54=1|60=20261023-12:00:00.000|38=100|40=2|44=72.5|"),
                       clock_now());
            c.flush();
            c.close();
            s.on_disconnected(clock_now());
            dropped = true;
            inbox.clear();
            ok = connect();
            ++reconnects;
            continue;
        }
        if (s.state() == session_state::active && submitted < n && submitted - answered < 64) {
            const std::string id = "O" + std::to_string(submitted);
            s.send("D", test::soh("11=" + id + "|21=1|55=VOD.L|54=1|60=20261023-12:00:00.000|38=100|40=2|44=72.5|"), clock_now());
            ++submitted;
        }
        if (answered == n && !logged_out && s.state() == session_state::active) {
            s.logout("", clock_now());
            logged_out = true;
        }
        c.flush();
    }
    ::kill(qf, SIGTERM);
    ::waitpid(qf, nullptr, 0);
    int distinct = 0;
    const bool ordered = in_order(k.reports, n, distinct);
    const int complaints = quickfix_complaints(dir);
    check(ok && ordered && reconnects == 1 && k.resend_requests >= 1 && k.poss_dups >= 1 && logged_out &&
              s.state() == session_state::disconnected,
          "S6 our initiator against QuickFIX's executor: " + std::to_string(distinct) + " of " + std::to_string(n) +
              " orders answered, in order, across 1 dropped connection (" + std::to_string(reconnects) +
              " reconnection), " + std::to_string(k.resend_requests) + " ResendRequest, " + std::to_string(k.poss_dups) +
              " reports resent with PossDup, logged out");
    check(k.rejects_sent == 0 && k.rejects_received == 0 && k.error_logouts == 0 && complaints == 0,
          "S6 our initiator against QuickFIX's executor: no Reject and no Logout for an error, either side (ours " +
              std::to_string(k.rejects_sent) + " sent, " + std::to_string(k.rejects_received) + " received; QuickFIX's log " +
              std::to_string(complaints) + ")");
}

void our_acceptor(const std::string& initiator, const std::string& dictionary, const std::string& dir) {
    std::filesystem::create_directories(dir);
    tcp_listener l;
    l.open(0);
    constexpr int n = 2000;
    const pid_t qf = spawn({initiator, settings(dir, false, l.port(), dictionary), std::to_string(n)}, dir + "/initiator.out");
    tcp_connection c;
    counting_sink k;
    k.c = &c;
    memory_store st;
    session_config cfg;
    cfg.role = session_role::acceptor;
    cfg.sender_comp_id = "VENUE";
    cfg.target_comp_id = "CLIENT";
    session<counting_sink, memory_store> s(cfg, k, st);
    int connections = 0, answered = 0, status = -1;
    bool dropped = false;
    std::string inbox;
    const std::int64_t give_up = clock_now().mono_ns + 90'000'000'000;
    while (clock_now().mono_ns < give_up) {
        if (!c.open()) {
            const int fd = l.accept();
            if (fd >= 0) {
                c.attach(fd);
                inbox.clear();
                s.on_connected(clock_now());
                ++connections;
            }
        }
        if (!dropped && answered >= 700 && c.open() && s.state() == session_state::active) {
            // Drop by our side with orders in flight: 50 ms unread, so what
            // QuickFIX sends meanwhile is lost with the socket and must be resent.
            c.flush();
            ::usleep(50'000);
            c.close();
            s.on_disconnected(clock_now());
            dropped = true;
            continue;
        }
        char b[16384];
        long r = 0;
        if (c.open())
            while ((r = c.read_some(b, sizeof(b))) > 0) inbox.append(b, static_cast<std::size_t>(r));
        if (!inbox.empty()) inbox.erase(0, s.on_bytes(inbox.data(), inbox.size(), clock_now()));
        if (r < 0) {
            c.close();
            s.on_disconnected(clock_now());
        }
        for (const auto& id : k.orders) {
            s.send("8", test::soh("37=X-" + id + "|11=" + id + "|17=E-" + id +
                                  "|150=0|39=0|55=VOD.L|54=1|38=100|151=100|14=0|6=0|"),
                   clock_now());
            ++answered;
        }
        k.orders.clear();
        s.on_tick(clock_now());
        if (c.open()) c.flush();
        int ws = 0;
        if (::waitpid(qf, &ws, WNOHANG) == qf) {
            status = WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
            break;
        }
    }
    if (status == -1) {
        ::kill(qf, SIGTERM);
        ::waitpid(qf, nullptr, 0);
    }
    std::ifstream out(dir + "/initiator.out");
    std::string summary, line;
    while (std::getline(out, line))
        if (line.starts_with("qf_initiator:")) summary = line;
    const int complaints = quickfix_complaints(dir);
    check(status == 0 && connections == 2 && k.resend_requests >= 1 && k.poss_dups >= 1, "S6 QuickFIX's initiator against our acceptor: every order answered, in order, across 1 dropped connection (" +
                                               std::to_string(connections) + " connections, " + std::to_string(k.resend_requests) + " ResendRequest from us, " +
                                               std::to_string(k.poss_dups) + " orders resent with PossDup); " + summary);
    check(k.rejects_sent == 0 && k.rejects_received == 0 && k.error_logouts == 0 && complaints == 0,
          "S6 QuickFIX's initiator against our acceptor: no Reject and no Logout for an error, either side (ours " +
              std::to_string(k.rejects_sent) + " sent, " + std::to_string(k.rejects_received) + " received; QuickFIX's log " +
              std::to_string(complaints) + ")");
}

// 3. Through the engine (note 007, S7): the pipeline, journaled, as initiator,
// the application submitting limit orders through the ring and logging out
// through it; QuickFIX's executor fills each. Its examples take no cancel or
// replace at FIX 4.4 (executor answers them with BusinessMessageReject 3,
// ordermatch every order-entry message), so those are not part of this.
void through_engine(const std::string& executor, const std::string& dictionary, const std::string& dir) {
    std::filesystem::create_directories(dir);
    const std::uint16_t port = free_port();
    const pid_t qf = spawn({executor, settings(dir, true, port, dictionary)}, dir + "/executor.out");
    ::usleep(200'000);  // the executor listening
    struct quiet {
        void log(session_log, std::string_view, std::uint64_t) {}
        bool resend(const message_view<>&) { return true; }
        bool release(std::string_view, std::string_view) { return true; }
    } q;
    session_config cfg;
    cfg.role = session_role::initiator;
    cfg.sender_comp_id = "CLIENT";
    cfg.target_comp_id = "VENUE";
    journal_file f;
    f.create(dir + "/engine.journal", 1, 8u << 20);
    auto in = std::make_unique<app_ring>(), out = std::make_unique<app_ring>();
    engine_config ec;
    ec.port = port;
    order_config oc;
    oc.max_orders = 4096;
    oc.max_exec_ids = 1 << 14;
    oc.id_bytes = 1 << 18;
    pipeline_engine<quiet> e(cfg, ec, f, q, *in, *out, {}, oc);
    std::atomic<bool> stop{false};
    std::thread t([&] { e.run(stop); });
    constexpr int n = 500;
    int submitted = 0, filled = 0, inconsistent = 0, refused = 0, others = 0;
    const std::int64_t give_up = clock_now().mono_ns + 30'000'000'000;
    bool logout_sent = false;
    while (!e.finished() && clock_now().mono_ns < give_up) {
        if (e.connected() && submitted < n && submitted - filled < 32) {
            char ts[utc_timestamp_length];
            format_utc_timestamp(clock_now().utc_ms, ts);
            if (submit_order(*in, "D", test::soh("11=E" + std::to_string(submitted) + "|21=1|55=VOD.L|54=1|60=" +
                                                  std::string(ts, sizeof(ts)) + "|38=100|40=2|44=72.5|")))
                ++submitted;
        }
        app_slot s;
        while (out->try_pop(s)) {
            if (s.kind == app_slot::order_state) {
                const order_event ev = slot_event(s);
                inconsistent += ev.inconsistent;
                filled += ev.ord_status == ord_status::filled;
            } else if (s.kind == app_slot::refused) {
                ++refused;
            } else if (s.kind == app_slot::message && std::string_view(s.data, s.length).find("\x01" "35=8\x01") == std::string_view::npos) {
                ++others;  // anything but an ExecutionReport: a BusinessMessageReject, say
            }
        }
        if (filled == n && !logout_sent) {
            submit_logout(*in);
            logout_sent = true;
        }
    }
    const bool finished = e.finished();
    stop = true;
    t.join();
    ::kill(qf, SIGTERM);
    ::waitpid(qf, nullptr, 0);
    const int complaints = quickfix_complaints(dir);
    check(finished && filled == n && inconsistent == 0 && refused == 0 && others == 0 && complaints == 0,
          "S7 (note 007) through the engine (pipeline, journaled) against QuickFIX's executor: " + std::to_string(filled) + " of " +
              std::to_string(n) + " limit orders Filled in our order book, " + std::to_string(inconsistent) +
              " inconsistent, no refusal, no message but ExecutionReports, logged out through the ring (" +
              (finished ? "engine finished" : "engine not finished") + "); QuickFIX's log " + std::to_string(complaints) +
              " complaints");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: interop_test <executor> <qf_initiator> <FIX44.xml>\n");
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    char t[] = "interop_test_XXXXXX";
    const char* d = ::mkdtemp(t);
    if (!d) return 2;
    our_initiator(argv[1], argv[3], std::string(d) + "/executor");
    our_acceptor(argv[2], argv[3], std::string(d) + "/initiator");
    through_engine(argv[1], argv[3], std::string(d) + "/engine");
    if (!test::failures) std::filesystem::remove_all(d);
    else std::printf("kept for inspection: %s\n", d);
    return test::result();
}
