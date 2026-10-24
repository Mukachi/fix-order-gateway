// Note 007, S3 (note 001, S7): the in-doubt cases of D6, each with the
// failure injected. Our initiator, the engine with its journal fsync'd, runs
// in a child process: the application submits 30 orders through the ring and
// cancels every fifth, then logs out and writes its order book's final state
// of each order to a file. The venue, a FIX session and the venue simulator,
// runs in the parent, over TCP, with a counterparty selling into our orders
// early in the run.
//  - kill: the child is killed with SIGKILL at random moments (1 to 3 times)
//    and a new one recovers from the journal: crashes before recording, and
//    after sending before the report (D6, 1 and 2);
//  - drop: the venue drops the connection at random moments, cancel-on-
//    disconnect on (D6, 3);
//  - stale: the venue drops the connection and holds off the reconnection
//    past the stale limit, losing what was in flight (D6, 4).
// Checked per run: every order's state in our book equals the venue's at our
// Logout; no NewOrderSingle reached the venue twice (F.1.a duplicates 0);
// none reached it older than the stale limit; an order reported not sent is
// unknown to the venue; the sequence never went back. Spec:
// docs/design/007-order-entry.md
#include "fix_text.hpp"

#include <fixgw/durability.hpp>
#include <fixgw/engine.hpp>
#include <fixgw/pipeline.hpp>
#include <fixgw/venue_sim.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace fixgw;
using fixgw::test::check;

constexpr int orders = 30;
constexpr std::size_t prefill = 1u << 20;
constexpr std::int64_t stale_ns = 300'000'000;

struct quiet {
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
// The stale limit: short for the stale mode; long otherwise, so a reconnect
// slowed by the backoff (1 s after a lost connection, note 005) tests D6's
// case 3, not case 4.
std::int64_t stale_limit_ns = stale_ns;

order_config book_config() {
    order_config c;
    c.max_orders = 1024;
    c.max_exec_ids = 1 << 14;
    c.id_bytes = 1 << 18;
    c.stale_ns = stale_limit_ns;
    return c;
}

std::string utc_now() {
    char ts[utc_timestamp_length];
    format_utc_timestamp(clock_now().utc_ms, ts);
    return std::string(ts, sizeof(ts));
}

// The orders an earlier child submitted: the journal's NewOrderSingle records.
int submitted_before(const std::string& path) {
    journal_reader r;
    if (!r.open(path)) return 0;
    int n = 0;
    for (const auto& rec : r.records())
        n += rec.kind == record_kind::send && rec.payload.size() >= 2 && rec.payload[0] == 1 && rec.payload[1] == 'D';
    return n;
}

// The application: submit, cancel every fifth, wait for quiet, log out, write
// the book's state of each order (and which it was told were not sent).
template <class Engine>
void application(Engine& e, app_ring& in, app_ring& out, int from, std::set<std::string>& not_sent, std::atomic<bool>& stop) {
    std::int64_t last_event = clock_now().mono_ns;
    const auto drain = [&] {
        app_slot s;
        while (out.try_pop(s)) {
            last_event = clock_now().mono_ns;
            if (s.kind == app_slot::order_state || s.kind == app_slot::replayed_state) {
                const order_event ev = slot_event(s);
                if (ev.kind == order_event::not_sent) not_sent.insert(std::string(ev.id()));
            }
        }
    };
    for (int i = from; i < orders; ++i) {
        const std::string id = "O" + std::to_string(i);
        while (!submit_order(in, "D", test::soh("11=" + id + "|55=VOD.L|54=1|60=" + utc_now() + "|38=100|40=2|44=50|"))) drain();
        if (i % 5 == 4) {
            const std::string orig = "O" + std::to_string(i - 2);
            while (!submit_order(in, "F", test::soh("41=" + orig + "|11=C" + std::to_string(i) + "|55=VOD.L|54=1|60=" +
                                                     utc_now() + "|38=100|")))
                drain();
        }
        for (int k = 0; k < 4; ++k) {
            drain();
            ::usleep(500);
        }
    }
    // quiet: nothing for 400 ms, connected, no order pending at the venue
    for (int k = 0; k < 20'000; ++k) {
        drain();
        if (e.connected() && clock_now().mono_ns - last_event > 400'000'000) break;
        ::usleep(1000);
    }
    submit_logout(in);
    for (int k = 0; k < 5000 && !e.finished(); ++k) {
        drain();
        ::usleep(1000);
    }
    stop = true;
}

template <class Engine>
void write_result(const Engine& e, const std::set<std::string>& not_sent, const std::string& path) {
    std::ofstream f(path + ".tmp");
    for (int i = 0; i < orders; ++i) {
        const std::string id = "O" + std::to_string(i);
        const auto st = e.session().orders().state(id);
        f << id << ' ' << (st ? st->ord_status : '-') << ' ' << (st ? st->cum_qty / fixed_one : 0) << ' '
          << (st ? st->leaves_qty / fixed_one : 0) << ' ' << (not_sent.count(id) ? 1 : 0) << '\n';
    }
    f.close();
    std::filesystem::rename(path + ".tmp", path);
}

[[noreturn]] void child(bool pipeline, const std::string& path, bool recovering, std::uint16_t port, const std::string& result) {
    recovering = recovering && std::filesystem::exists(path);
    quiet q;
    auto in = std::make_unique<app_ring>(), out = std::make_unique<app_ring>();
    engine_config ec;
    ec.port = port;
    const int before = recovering ? submitted_before(path) : 0;
    std::atomic<bool> stop{false};
    std::set<std::string> not_sent;
    if (pipeline) {
        journal_file f;
        if (!recovering) {
            if (!f.create(path, 1, prefill)) ::_exit(16);
        } else {
            journal_reader r;
            if (!r.open(path) || !r.cut() || !f.open_append(path, r.valid_end(), prefill)) ::_exit(16);
        }
        pipeline_engine<quiet> p(ours(), ec, f, q, *in, *out, {}, book_config());
        if (recovering) {
            journal_reader r;
            if (!r.open(path) || !p.recover(r)) ::_exit(12);
        }
        std::thread t([&] { p.run(stop); });
        application(p, *in, *out, before, not_sent, stop);
        t.join();
        write_result(p, not_sent, result);
    } else {
        fsync_each j;
        if (!recovering) {
            if (!j.create(path, 1, prefill)) ::_exit(16);
        } else {
            journal_reader r;
            if (!r.open(path) || !r.cut() || !j.open_append(path, r.valid_end(), prefill)) ::_exit(16);
        }
        engine<fsync_each, quiet> e(ours(), ec, j, q, *in, *out, book_config());
        if (recovering) {
            journal_reader r;
            if (!r.open(path) || !e.recover(r)) ::_exit(12);
        }
        std::thread t([&] { e.run(stop); });
        application(e, *in, *out, before, not_sent, stop);
        t.join();
        write_result(e, not_sent, result);
    }
    ::_exit(0);
}

struct venue_sink {
    tcp_connection* c = nullptr;
    venue_sim* sim = nullptr;
    std::function<void(std::string_view, const std::string&)> send;  // through the venue's session
    std::int64_t stale_arrivals = 0;
    bool too_low = false;
    void write(std::string_view b) {
        if (c->open()) c->write(b);
    }
    void disconnect() { c->close(); }
    void deliver(const message_view<>& m) {
        if (m.msg_type() == "D") {  // older than the stale limit on arrival: D5 broken
            const auto sent = detail::parse_utc_timestamp(m.get(60).value_or(""));
            if (sent && clock_now().utc_ms - *sent > stale_limit_ns / 1'000'000 + 100) ++stale_arrivals;
        }
        sim->on_message(1, m, [&](int s, std::string_view t, const std::string& b) {
            if (s == 1) send(t, b);
        });
    }
    void log(session_log, std::string_view what, std::uint64_t) {
        if (what.find("MsgSeqNum too low") != std::string_view::npos) too_low = true;
    }
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
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

enum class mode { kill, drop, stale };
const char* name(mode m) { return m == mode::kill ? "killed" : m == mode::drop ? "connection dropped" : "stale resend"; }

struct outcome {
    bool completed = false;
    std::string why;
    int not_sent = 0, failures = 0, kills = 0, drops = 0, logouts = 0;
    std::map<char, int> venue_states;  // the venue's final OrdStatus of each order: how far the run reached
};

outcome run(bool pipeline, mode md, std::mt19937& rng, const std::string& dir) {
    stale_limit_ns = md == mode::stale ? stale_ns : 60'000'000'000;  // read by the child after fork
    const std::string path = dir + "/indoubt.journal", result = dir + "/result.txt";
    std::filesystem::remove(path);
    std::filesystem::remove(result);
    tcp_listener l;
    l.open(0);
    venue_config vcfg;
    vcfg.cancel_on_disconnect = md != mode::kill;
    venue_sim sim(vcfg);
    tcp_connection pc;
    venue_store vst;
    session_config vc;
    vc.role = session_role::acceptor;
    vc.sender_comp_id = "VENUE";
    vc.target_comp_id = "CLIENT";
    venue_sink vs;
    vs.c = &pc;
    vs.sim = &sim;
    session<venue_sink, venue_store> peer(vc, vs, vst);
    vs.send = [&](std::string_view t, const std::string& b) { peer.send(t, b, clock_now()); };
    outcome o;
    std::map<std::string, venue_sim::order_view> truth;
    bool truth_taken = false, was_replied = false;
    const std::int64_t run_start = clock_now().mono_ns;
    std::int64_t truth_at_ms = 0, run_ms = 0;
    const int kills = md == mode::kill ? 1 + static_cast<int>(rng() % 3) : 0;
    int takers = 0;
    std::int64_t next_taker = 0, hold_until = 0;
    int drops = md == mode::kill ? 0 : 1 + static_cast<int>(rng() % 2);
    for (int g = 0; g <= kills; ++g) {
        std::fflush(stdout);  // or the child inherits, and repeats, unwritten output
        const pid_t pid = ::fork();
        if (pid == 0) child(pipeline, path, g > 0, l.port(), result);
        bool killing = g < kills;
        const bool to_kill = killing;
        const std::int64_t start = clock_now().mono_ns;
        const std::int64_t kill_at = start + static_cast<std::int64_t>(rng() % 60'000'000);
        std::int64_t drop_at = start + 5'000'000 + static_cast<std::int64_t>(rng() % 50'000'000);
        std::string inbox;
        int status = 0;
        for (;;) {
            const session_time now = clock_now();
            if (!pc.open() && now.mono_ns >= hold_until) {
                const int fd = l.accept();
                if (fd >= 0) {
                    pc.attach(fd);
                    inbox.clear();
                    peer.on_connected(now);
                }
            }
            if (pc.open()) {
                char b[16384];
                long n;
                while ((n = pc.read_some(b, sizeof(b))) > 0) inbox.append(b, static_cast<std::size_t>(n));
                if (!inbox.empty()) inbox.erase(0, peer.on_bytes(inbox.data(), inbox.size(), now));
                // Each Logout of ours: the venue's view then; the last one is the
                // application's (a Logout the session sends to end a bad
                // connection may come earlier, and recovery follows it).
                const bool replied = peer.state() == session_state::logout_replied;
                if (replied && !was_replied) {
                    truth.clear();
                    for (int i = 0; i < orders; ++i)
                        if (const auto v = sim.state(1, "O" + std::to_string(i))) truth["O" + std::to_string(i)] = *v;
                    truth_taken = true;
                    ++o.logouts;
                    truth_at_ms = (now.mono_ns - run_start) / 1'000'000;
                }
                was_replied = replied;
                if (n < 0) {
                    pc.close();
                    peer.on_disconnected(now);
                    sim.on_disconnect(1, [&](int s, std::string_view t, const std::string& body) {
                        if (s == 1) peer.send(t, body, now);  // queued for the next logon (scenario 16)
                    });
                }
            }
            if (pc.open() && takers < 30 && now.mono_ns >= next_taker && peer.state() == session_state::active) {
                const std::string m = test::message("35=D|49=OTHER|56=VENUE|34=1|52=20261021-07:30:00.000|11=S" +
                                                    std::to_string(++takers) + "|55=VOD.L|54=2|60=20261021-07:30:00.000|38=" +
                                                    std::to_string(50 * (1 + rng() % 3)) + "|40=2|44=50|59=3|");
                message_view<> v;
                decode(m.data(), m.size(), v);
                sim.on_message(2, v, [&](int s, std::string_view t, const std::string& body) {
                    if (s == 1) peer.send(t, body, now);
                });
                next_taker = now.mono_ns + 3'000'000;
            }
            if (drops > 0 && now.mono_ns >= drop_at && pc.open() && peer.state() == session_state::active) {
                pc.close();  // whatever was in flight to us, or unread from them, is lost
                inbox.clear();
                peer.on_disconnected(now);
                sim.on_disconnect(1, [&](int s, std::string_view t, const std::string& body) {
                    if (s == 1) peer.send(t, body, now);
                });
                if (md == mode::stale) hold_until = now.mono_ns + 2 * stale_ns;
                --drops;
                ++o.drops;
                drop_at = now.mono_ns + 20'000'000 + static_cast<std::int64_t>(rng() % 40'000'000);
            }
            if (pc.open()) pc.flush();
            peer.on_tick(now);
            if (killing && now.mono_ns >= kill_at) {
                ::kill(pid, SIGKILL);
                killing = false;
                ++o.kills;
            }
            if (::waitpid(pid, &status, WNOHANG) == pid) break;
            if (now.mono_ns - start > 30'000'000'000) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, &status, 0);
                o.why = "timed out";
                break;
            }
        }
        if (pc.open()) {
            pc.close();
            peer.on_disconnected(clock_now());
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            o.completed = true;
            break;
        }
        if (!WIFSIGNALED(status) || !to_kill) {
            if (o.why.empty()) o.why = "child failed, status " + std::to_string(status);
            return o;
        }
    }
    run_ms = (clock_now().mono_ns - run_start) / 1'000'000;
    if (!o.completed) {
        if (o.why.empty()) o.why = "did not complete";
        return o;
    }
    std::ifstream f(result);
    std::string id;
    char st;
    long cum, leaves;
    int ns;
    int seen = 0;
    while (f >> id >> st >> cum >> leaves >> ns) {
        ++seen;
        o.not_sent += ns;
        const auto it = truth.find(id);
        std::string bad;
        if (ns) {
            if (it != truth.end()) bad = "reported not sent, but the venue has it";
        } else if (st == '-') {
            if (it != truth.end()) bad = "the venue has it, our book does not";
        } else if (it == truth.end()) {
            if (st != ord_status::rejected)
                bad = "our book has it (" + std::string(1, st) + "), the venue does not at our Logout (" +
                      (sim.state(1, id) ? "it does at the end" : "nor at the end") + "; " + std::to_string(o.logouts) +
                      " Logouts, the view taken at +" + std::to_string(truth_at_ms) + " ms of " + std::to_string(run_ms) + ")";
        } else if (it->second.status != st || it->second.cum / fixed_one != cum || it->second.leaves / fixed_one != leaves) {
            bad = std::string("ours ") + st + " " + std::to_string(cum) + "/" + std::to_string(leaves) + ", the venue's " +
                  it->second.status + " " + std::to_string(it->second.cum / fixed_one) + "/" +
                  std::to_string(it->second.leaves / fixed_one);
        }
        if (!bad.empty()) {
            ++o.failures;
            if (o.why.empty()) o.why = id + ": " + bad;
        }
    }
    if (seen != orders && o.why.empty()) o.why = "result has " + std::to_string(seen) + " orders";
    if (sim.duplicates() > 0 && o.why.empty()) o.why = std::to_string(sim.duplicates()) + " orders reached the venue twice";
    if (vs.stale_arrivals > 0 && o.why.empty()) o.why = std::to_string(vs.stale_arrivals) + " orders reached the venue stale";
    if (vs.too_low && o.why.empty()) o.why = "the sequence went back";
    if (!truth_taken && o.why.empty()) o.why = "no Logout seen";
    for (const auto& [k, v] : truth) ++o.venue_states[v.status];
    return o;
}

}  // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    char t[] = "indoubt_test_XXXXXX";
    const char* d = ::mkdtemp(t);
    if (!d) return 1;
    std::mt19937 rng(7);
    struct plan {
        bool pipeline;
        mode md;
    };
    for (const plan p : {plan{true, mode::kill}, plan{false, mode::kill}, plan{true, mode::drop}, plan{true, mode::stale}}) {
        int good = 0, not_sent = 0, kills = 0, drops = 0;
        std::map<char, int> states;
        constexpr int runs = 12;
        std::string first;
        for (int r = 0; r < runs; ++r) {
            const outcome o = run(p.pipeline, p.md, rng, d);
            const bool ok = o.completed && o.why.empty();
            good += ok;
            not_sent += o.not_sent;
            kills += o.kills;
            drops += o.drops;
            for (const auto& [k, v] : o.venue_states) states[k] += v;
            if (!ok && first.empty()) first = "run " + std::to_string(r) + ": " + o.why;
        }
        const std::string what = std::string(p.pipeline ? "pipeline" : "direct loop") + ", " + name(p.md);
        std::string reach;
        for (const auto& [k, v] : states) reach += (reach.empty() ? "" : " ") + std::to_string(v) + "x" + k;
        check(good == runs, "S3 " + what + ": " + std::to_string(runs) + " runs of 30 orders (" + std::to_string(kills) + " SIGKILLs, " +
                                std::to_string(drops) + " drops; the venue's final OrdStatus " + reach +
                                "), every order's state equal to the venue's, none twice, none stale" +
                                (p.md == mode::stale ? ", " + std::to_string(not_sent) + " reported not sent and unknown to the venue" : "") +
                                (good == runs ? "" : "; " + std::to_string(runs - good) + " broke, first " + first));
    }
    std::filesystem::remove_all(d);
    return test::result();
}
