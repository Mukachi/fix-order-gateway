// Note 005, S2: note 004's guarantee through real sockets, for both the
// direct loop and the pipeline. Our initiator runs in a child process (the
// engine on its thread, the application submitting orders through its ring)
// and is killed with SIGKILL at a random moment, wherever it is: in an
// fdatasync, in a send, between stages. A new child recovers from the journal
// and carries on. Power loss is simulated by zeroing the journal past the last
// position the child reported durable; it reports before anything waiting on
// that position can move. The counterparty, a plain acceptor in the parent,
// sees every frame. Checked per run: no MsgSeqNum carried two different
// messages; every order arrived exactly once; the sequence never went back.
#include "fix_text.hpp"

#include <fixgw/durability.hpp>
#include <fixgw/engine.hpp>
#include <fixgw/pipeline.hpp>

#include <atomic>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace fixgw;
using fixgw::test::check;

constexpr int orders = 30;
constexpr std::size_t prefill = 1u << 20;

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

int report_fd = -1;
void report(std::size_t end) {
    const std::uint64_t v = end;
    if (::write(report_fd, &v, sizeof(v)) < 0) ::_exit(10);
}

// The direct loop's strategy: A, reporting each durable position before the
// engine releases anything.
struct reporting_fsync {
    fsync_each in;
    bool append(record_kind k, session_time t, std::string_view a, std::string_view b) { return in.append(k, t, a, b); }
    durability make_durable(session_time t) {
        const durability d = in.make_durable(t);
        if (d == durability::durable) report(in.end());
        return d;
    }
    durability poll(session_time t) { return in.poll(t); }
};

// Orders already submitted by an earlier child: the journal's send records.
int submitted_before(const std::string& path) {
    journal_reader r;
    if (!r.open(path)) return 0;
    int n = 0;
    for (const auto& rec : r.records()) n += rec.kind == record_kind::send;
    return n;
}

void submit_rest(app_ring& in, int from, int doorbell) {
    for (int i = from; i < orders; ++i) {
        while (!submit_order(in, "D", test::soh("11=O" + std::to_string(i) + "|55=VOD.L|54=1|38=100|40=1|"), doorbell)) {
        }
        ::usleep(500);
    }
    ::usleep(300'000);  // let the last ones go out
}

[[noreturn]] void child_direct(const std::string& path, bool recovering, std::uint16_t port) {
    reporting_fsync j;
    recovering = recovering && std::filesystem::exists(path);  // killed before the journal existed: start afresh
    if (!recovering) {
        if (!j.in.create(path, 1, prefill)) ::_exit(16);
    } else {
        journal_reader r;
        if (!r.open(path) || !r.cut() || !j.in.open_append(path, r.valid_end(), prefill)) ::_exit(16);
    }
    quiet q;
    auto in = std::make_unique<app_ring>(), out = std::make_unique<app_ring>();
    engine_config ec;
    ec.port = port;
    engine<reporting_fsync, quiet> e(ours(), ec, j, q, *in, *out);
    if (recovering) {
        journal_reader r;
        if (!r.open(path) || !e.recover(r)) ::_exit(12);
    }
    const int before = recovering ? submitted_before(path) : 0;
    std::atomic<bool> stop{false};
    std::thread t([&] { e.run(stop); });
    for (int k = 0; k < 2000 && !e.connected(); ++k) ::usleep(1000);
    submit_rest(*in, before, -1);
    stop = true;
    t.join();
    ::_exit(0);
}

[[noreturn]] void child_pipeline(const std::string& path, bool recovering, std::uint16_t port) {
    journal_file f;
    recovering = recovering && std::filesystem::exists(path);  // killed before the journal existed: start afresh
    if (!recovering) {
        if (!f.create(path, 1, prefill)) ::_exit(16);
    } else {
        journal_reader r;
        if (!r.open(path) || !r.cut() || !f.open_append(path, r.valid_end(), prefill)) ::_exit(16);
    }
    quiet q;
    auto in = std::make_unique<app_ring>(), out = std::make_unique<app_ring>();
    engine_config ec;
    ec.port = port;
    pipeline_engine<quiet> p(ours(), ec, f, q, *in, *out);
    p.on_durable([](void*, std::size_t end) { report(end); }, nullptr);
    if (recovering) {
        journal_reader r;
        if (!r.open(path) || !p.recover(r)) ::_exit(12);
    }
    const int before = recovering ? submitted_before(path) : 0;
    std::atomic<bool> stop{false};
    std::thread t([&] { p.run(stop); });
    for (int k = 0; k < 2000 && !p.connected(); ++k) ::usleep(1000);
    submit_rest(*in, before, -1);
    stop = true;
    t.join();
    ::_exit(0);
}

struct peer_sink {
    tcp_connection* c = nullptr;
    std::set<std::string> ids;
    int duplicates = 0;
    bool too_low = false;
    void write(std::string_view b) {
        if (c->open()) c->write(b);
    }
    void disconnect() { c->close(); }
    void deliver(const message_view<>& m) {
        if (!ids.insert(std::string(m.get(11).value_or(""))).second) ++duplicates;
    }
    std::string too_low_text;
    void log(session_log, std::string_view what, std::uint64_t) {
        if (what.find("MsgSeqNum too low") != std::string_view::npos) {
            too_low = true;
            too_low_text = std::string(what);
        }
    }
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

std::string content(const message_view<>& v) {
    std::string c(v.msg_type());
    for (std::size_t i = 0; i < v.size(); ++i) {
        const auto t = v[i].tag;
        if (detail::is_header_tag(t) || detail::is_trailer_tag(t)) continue;
        c.append("|").append(std::to_string(t)).append("=").append(v.value(i));
    }
    return c;
}

struct outcome {
    bool completed = false, conflict = false, too_low = false;
    std::string why;
    int delivered = 0, duplicates = 0, kills = 0;
};

// One run: `kills` children killed after random delays, then one left to
// finish; with `power_loss`, the journal zeroed past the last durable report
// after each kill.
outcome run(bool pipeline, bool power_loss, int kills, std::mt19937& rng, const std::string& dir) {
    const std::string path = dir + "/engine.journal";
    std::filesystem::remove(path);
    tcp_listener l;
    l.open(0);
    peer_sink ps;
    tcp_connection pc;
    ps.c = &pc;
    peer_store pst;
    session_config vc;
    vc.role = session_role::acceptor;
    vc.sender_comp_id = "VENUE";
    vc.target_comp_id = "CLIENT";
    session<peer_sink, peer_store> peer(vc, ps, pst);
    std::map<std::uint64_t, std::string> seen;
    outcome o;
    std::uint64_t durable = journal_format::header_size;
    for (int g = 0; g <= kills; ++g) {
        int rp[2];
        if (::pipe2(rp, O_CLOEXEC | O_NONBLOCK) != 0) break;
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::close(rp[0]);
            report_fd = rp[1];
            if (pipeline) child_pipeline(path, g > 0, l.port());
            child_direct(path, g > 0, l.port());
        }
        ::close(rp[1]);
        const bool killing = g < kills;
        const std::int64_t kill_at = clock_now().mono_ns + static_cast<std::int64_t>(rng() % 25'000'000);  // 0-25 ms
        std::string inbox;
        bool exited = false;
        int status = 0;
        while (!exited) {
            if (!pc.open()) {
                const int fd = l.accept();
                if (fd >= 0) {
                    pc.attach(fd);
                    inbox.clear();
                    peer.on_connected(clock_now());
                }
            }
            if (pc.open()) {
                char b[4096];
                long n;
                while ((n = pc.read_some(b, sizeof(b))) > 0) {
                    inbox.append(b, static_cast<std::size_t>(n));
                    for (std::size_t at = 0;;) {  // every whole frame, before the session sees it
                        const auto f = frame(inbox.data() + at, inbox.size() - at, 1 << 16);
                        if (f.status != frame_status::complete) break;
                        message_view<> v;
                        if (decoder<256>::decode(inbox.data() + at, f.length, v).ok &&
                            !(v.msg_type() == "4" && v.get(123) == std::string_view("Y"))) {
                            const auto seq = detail::parse_uint(v.get(34).value_or(""));
                            if (seq) {
                                const auto [it, fresh] = seen.emplace(*seq, content(v));
                                if (!fresh && it->second != content(v)) o.conflict = true;
                            }
                        }
                        at += f.length;
                    }
                    inbox.erase(0, peer.on_bytes(inbox.data(), inbox.size(), clock_now()));
                }
                if (n < 0) {
                    pc.close();
                    peer.on_disconnected(clock_now());
                }
                pc.flush();
            }
            peer.on_tick(clock_now());
            std::uint64_t d;
            while (::read(rp[0], &d, sizeof(d)) == sizeof(d)) durable = d;
            if (killing && clock_now().mono_ns >= kill_at) ::kill(pid, SIGKILL);
            if (::waitpid(pid, &status, WNOHANG) == pid) exited = true;
        }
        std::uint64_t d;
        while (::read(rp[0], &d, sizeof(d)) == sizeof(d)) durable = d;
        ::close(rp[0]);
        if (pc.open()) {
            pc.close();
            peer.on_disconnected(clock_now());
        }
        if (!killing) {
            o.completed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            break;
        }
        if (!WIFSIGNALED(status)) {  // it finished, or failed, before the kill
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                o.completed = true;
                break;
            }
            break;
        }
        ++o.kills;
        if (power_loss && std::filesystem::exists(path)) {  // no journal yet: nothing to lose
            const int fd = ::open(path.c_str(), O_WRONLY);
            struct stat st;
            if (fd < 0 || ::fstat(fd, &st) != 0) break;
            const std::size_t size = static_cast<std::size_t>(st.st_size);
            const std::size_t n = size > durable ? size - durable : 0;
            const std::string zeros(n, '\0');
            const bool ok = n == 0 || ::pwrite(fd, zeros.data(), n, static_cast<off_t>(durable)) == static_cast<ssize_t>(n);
            ::close(fd);
            if (!ok) break;
        }
    }
    o.delivered = static_cast<int>(ps.ids.size());
    o.duplicates = ps.duplicates;
    o.too_low = ps.too_low;
    o.why = ps.too_low_text;
    return o;
}

}  // namespace

int main() {
    char t[] = "engine_crash_test_XXXXXX";
    const char* d = ::mkdtemp(t);
    if (!d) {
        check(false, "temporary directory");
        return test::result();
    }
    std::mt19937 rng(11);
    for (const bool pipeline : {false, true})
        for (const bool power : {false, true}) {
            int runs = 0, good = 0, killed = 0;
            std::string first_bad;
            for (int r = 0; r < 25; ++r) {
                const auto o = run(pipeline, power, 1 + r % 3, rng, d);
                ++runs;
                killed += o.kills;
                const bool ok = o.completed && !o.conflict && !o.too_low && o.delivered == orders && o.duplicates == 0;
                good += ok;
                if (!ok && first_bad.empty())
                    first_bad = "run " + std::to_string(r) + (o.completed ? "" : " (did not complete)") +
                                (o.conflict ? " (MsgSeqNum conflict)" : "") + (o.too_low ? " (sequence went back: " + o.why + ")" : "") +
                                " delivered " + std::to_string(o.delivered) + " dup " + std::to_string(o.duplicates);
            }
            check(good == runs, std::string("S2 ") + (pipeline ? "pipeline" : "direct loop") +
                                    (power ? ", simulated power loss" : ", killed") + ": " + std::to_string(runs) + " runs, " +
                                    std::to_string(killed) + " SIGKILLs at random moments" +
                                    (good == runs ? ", all correct" : ", " + std::to_string(runs - good) + " broke; first: " + first_bad));
        }
    std::filesystem::remove_all(d);
    return test::result();
}
