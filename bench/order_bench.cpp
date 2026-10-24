// Order-to-ExecutionReport latency through our gateway (note 008, D1-D5):
// the application submits limit NewOrderSingles on a fixed schedule (open
// loop) through the engine to order_venue and stamps each ExecutionReport
// (New) as it reaches the application's ring; latency from each order's
// scheduled time. Bodies are built before the schedule starts, so the
// application's own string building is not counted. Configurations, each
// with the guarantee it gives (note 004):
//   pipeline, journal A, busy-poll   survives power loss (the default)
//   direct loop, journal A, blocking survives power loss
//   direct loop, journal C, busy-poll survives a process crash
// Rates 1,000 and 10,000 a second, then 20,000, 50,000, 100,000 until one is
// not sustained. 100,000 orders a point after 5,000 of warm-up. After each
// point the venue's own turnaround (order_venue's file) goes into the run.
// Run JSON through lockfree-ipc's harness. Design: docs/design/008-measurement.md
//
// Usage: order_bench <venue port> <venue dir> [output.json [--quick]]
// --quick (10,000 orders a point) only with an output path outside results/.
// order_venue must be serving on the port with <venue dir>, enough connections.
#include <fixgw/durability.hpp>
#include <fixgw/engine.hpp>
#include <fixgw/pipeline.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/overhead.hpp>
#include <lfipc/bench/run_file.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace lfipc::bench;
namespace fx = fixgw;

constexpr int app_cpu = 2, engine_cpu = 4, journal_cpu = 6, sender_cpu = 8;
std::size_t samples = 100'000, warmup = 5'000;  // --quick: 10,000 and 1,000, for check runs only
constexpr std::size_t journal_bytes = 256u << 20;

struct quiet {
    void log(fx::session_log, std::string_view, std::uint64_t) {}
    bool resend(const fx::message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};

fx::session_config client() {
    fx::session_config c;
    c.role = fx::session_role::initiator;
    c.sender_comp_id = "CLIENT";
    c.target_comp_id = "VENUE";
    return c;
}
fx::order_config book() {
    fx::order_config c;
    c.max_orders = 1u << 18;
    c.max_exec_ids = 1u << 18;
    c.id_bytes = 16u << 20;
    return c;
}

struct venue_figures {
    bool read = false;
    std::size_t samples = 0;
    double p50 = 0, p99 = 0, p999 = 0, max = 0;
};

struct point {
    point(std::string l, std::string j, std::string w, std::string g, double r)
        : loop(std::move(l)), journal(std::move(j)), wait(std::move(w)), guarantee(std::move(g)), rate(r) {}
    std::string gateway = "ours", loop, journal, wait, guarantee;
    double rate = 0;
    overhead cost;
    double sustained = 0;
    bool complete = false;
    venue_figures venue;
};

// The venue's figures for its connection `k`, once it has written them.
venue_figures read_venue(const std::string& dir, int k) {
    venue_figures v;
    const std::string path = dir + "/venue-" + std::to_string(k) + ".txt";
    for (int i = 0; i < 30000 && !std::filesystem::exists(path); ++i) ::usleep(1000);  // a point that fell behind: the venue drains it first
    std::ifstream f(path);
    std::string w;
    double conn;
    if (f >> w >> conn >> w >> v.samples >> w >> v.p50 >> w >> v.p99 >> w >> v.p999 >> w >> v.max) v.read = true;
    return v;
}

// The application's side of a point.
template <class Connected>
void drive(fx::app_ring& in, fx::app_ring& out, double rate, double tpn, Connected connected, point& r) {
    const std::size_t n = warmup + samples;
    std::vector<std::string> bodies(n);
    for (std::size_t i = 0; i < n; ++i)
        bodies[i] = "11=" + std::to_string(i) + "\x01" "21=1\x01" "55=VOD.L\x01" "54=1\x01" "60=20261024-00:00:00.000\x01"
                    "38=100\x01" "40=2\x01" "44=72.53\x01";
    std::vector<ticks> due(n, 0), back(n, 0);
    for (int k = 0; k < 50'000'000 && !connected(); ++k) {
    }
    std::size_t received = 0;
    const auto collect = [&] {
        fx::app_slot s;
        while (out.try_pop(s)) {
            if (s.kind != fx::app_slot::message) continue;
            const std::string_view m(s.data, s.length);
            const auto at = m.find("\x01" "11=");
            if (at == std::string_view::npos || m.find("\x01" "35=8\x01") == std::string_view::npos) continue;
            const std::size_t id = std::strtoull(m.data() + at + 4, nullptr, 10);
            if (id < n && back[id] == 0) {
                back[id] = stop();
                ++received;
            }
        }
    };
    const ticks interval = static_cast<ticks>(1e9 / rate * tpn);
    ticks t0 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (i == 0) {  // the first waits for the session; the schedule starts when it is back
            due[0] = start();
            while (!fx::submit_order(in, "D", bodies[0])) {
            }
            for (int k = 0; k < 200'000'000 && received == 0; ++k) collect();
            t0 = start() + interval;
            continue;
        }
        due[i] = t0 + interval * (i - 1);
        while (start() < due[i]) collect();
        while (!fx::submit_order(in, "D", bodies[i])) collect();
    }
    const ticks give_up = start() + static_cast<ticks>(10e9 * tpn);
    while (received < n && start() < give_up) collect();
    std::vector<ticks> lat;
    lat.reserve(samples);
    ticks last = 0;
    for (std::size_t i = warmup; i < n; ++i) {
        lat.push_back(back[i] > due[i] ? back[i] - due[i] : 0);
        if (back[i] > last) last = back[i];
    }
    r.cost = summarise_overhead(std::move(lat));
    r.cost.warmup_discarded = warmup;
    r.cost.cpu = app_cpu;
    r.complete = received == n;
    if (r.complete && last > due[warmup])
        r.sustained = static_cast<double>(samples) / (static_cast<double>(last - due[warmup]) / tpn / 1e9);
}

point pipeline_a(std::uint16_t port, double rate, double tpn, const std::string& dir, int k) {
    point r("pipeline", "A", "busy-poll", "power loss", rate);
    fx::journal_file f;
    const std::string path = dir + "/p" + std::to_string(k) + ".journal";
    if (!f.create(path, 1, journal_bytes)) return r;
    quiet q;
    auto in = std::make_unique<fx::app_ring>(), out = std::make_unique<fx::app_ring>();
    fx::engine_config ec;
    ec.port = port;
    fx::pipeline_engine<quiet> p(client(), ec, f, q, *in, *out, {engine_cpu, journal_cpu, sender_cpu}, book());
    std::atomic<bool> stop_flag{false};
    std::thread t([&] { p.run(stop_flag); });
    drive(*in, *out, rate, tpn, [&] { return p.connected(); }, r);
    stop_flag = true;
    t.join();
    std::filesystem::remove(path);
    return r;
}

template <class Journal>
point direct(std::uint16_t port, double rate, double tpn, const std::string& dir, int k, fx::wait_mode wait, const char* name) {
    point r("direct", name, wait == fx::wait_mode::busy ? "busy-poll" : "block",
            std::string(name) == "A" ? "power loss" : "process crash", rate);
    Journal j;
    const std::string path = dir + "/d" + std::to_string(k) + ".journal";
    if (!j.create(path, 1, journal_bytes)) return r;
    quiet q;
    auto in = std::make_unique<fx::app_ring>(), out = std::make_unique<fx::app_ring>();
    fx::engine_config ec;
    ec.wait = wait;
    ec.port = port;
    fx::engine<Journal, quiet> e(client(), ec, j, q, *in, *out, book());
    std::atomic<bool> stop_flag{false};
    std::thread t([&] {
        detail::pin_to(engine_cpu);
        e.run(stop_flag);
    });
    drive(*in, *out, rate, tpn, [&] { return e.connected(); }, r);
    stop_flag = true;
    t.join();
    std::filesystem::remove(path);
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: order_bench <venue port> <venue dir> [output.json]\n");
        return 2;
    }
    const auto port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    const std::string venue_dir = argv[2];
    if (argc > 4 && std::string(argv[4]) == "--quick") {
        if (std::string(argv[3]).find("/results/") != std::string::npos) {
            std::fprintf(stderr, "order_bench: --quick is for check runs, not results/\n");
            return 2;
        }
        samples = 10'000;
        warmup = 1'000;
    }
    char* run_argv[] = {argv[0], argc > 3 ? argv[3] : nullptr, nullptr};
    run_file run(argc > 3 ? 2 : 1, run_argv, LFIPC_RESULTS_DIR, "order");
    if (run.refused) return run.finish(false, "order_bench");
    const auto noise = sample_noise({app_cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !detail::pin_to(app_cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "order_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "order_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    char tmpl[] = "order_bench_journals_XXXXXX";
    const char* dir_c = ::mkdtemp(tmpl);
    const std::string dir = dir_c ? std::filesystem::absolute(dir_c).string() : ".";
    std::vector<point> points;
    int k = 0;
    const auto run_point = [&](auto&& measure, double rate) {
        point p = measure(rate, k);
        p.venue = read_venue(venue_dir, k);
        ++k;
        std::printf("%-5s %-8s %s %-9s %7.0f/s %s: p50 %9.1f p99 %9.1f p99.9 %9.1f max %10.1f ns  sustained %8.0f  venue p50 %6.0f ns\n",
                    p.gateway.c_str(), p.loop.c_str(), p.journal.c_str(), p.wait.c_str(), p.rate, p.complete ? "   " : "INC",
                    static_cast<double>(p.cost.p50) / tpn, static_cast<double>(p.cost.p99) / tpn,
                    static_cast<double>(p.cost.p999) / tpn, static_cast<double>(p.cost.max) / tpn, p.sustained, p.venue.p50);
        std::fflush(stdout);
        points.push_back(p);
        return p.complete && p.sustained >= 0.95 * rate;
    };
    const auto ladder = [&](auto&& measure) {
        for (const double rate : {1'000.0, 10'000.0}) run_point(measure, rate);
        for (const double rate : {20'000.0, 50'000.0, 100'000.0})
            if (!run_point(measure, rate)) break;
    };
    ladder([&](double rate, int i) { return pipeline_a(port, rate, tpn, dir, i); });
    ladder([&](double rate, int i) { return direct<fx::fsync_each>(port, rate, tpn, dir, i, fx::wait_mode::block, "A"); });
    ladder([&](double rate, int i) { return direct<fx::mapped_journal>(port, rate, tpn, dir, i, fx::wait_mode::busy, "C"); });
    if (dir_c) std::filesystem::remove_all(dir);

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto ns = [&](ticks t) { return static_cast<double>(t) / tpn; };
    run.w.key("order");
    run.w.begin_object();
    run.w.member("gateway", std::string("ours"));
    run.w.member("path", std::string("loopback"));
    run.w.member("quick", samples != 100'000);
    run.w.member("app_cpu", app_cpu);
    run.w.member("engine_cpu", engine_cpu);
    run.w.member("journal_cpu", journal_cpu);
    run.w.member("sender_cpu", sender_cpu);
    run.w.member("lockfree_ipc_commit", FIXGW_LFIPC_COMMIT);
    run.w.key("points");
    run.w.begin_array();
    for (const auto& p : points) {
        run.w.begin_object();
        run.w.member("gateway", p.gateway);
        run.w.member("loop", p.loop);
        run.w.member("journal", p.journal);
        run.w.member("wait", p.wait);
        run.w.member("survives", p.guarantee);
        run.w.member("rate_per_s", p.rate);
        run.w.member("complete", p.complete);
        run.w.member("samples", p.cost.samples);
        run.w.member("warmup_discarded", p.cost.warmup_discarded);
        run.w.member("p50_ns", ns(p.cost.p50));
        run.w.member("p99_ns", ns(p.cost.p99));
        run.w.member("p999_ns", ns(p.cost.p999));
        run.w.member("max_ns", ns(p.cost.max));
        run.w.member("sustained_per_s", p.sustained);
        run.w.member("venue_read", p.venue.read);
        run.w.member("venue_samples", p.venue.samples);
        run.w.member("venue_p50_ns", p.venue.p50);
        run.w.member("venue_p99_ns", p.venue.p99);
        run.w.member("venue_p999_ns", p.venue.p999);
        run.w.member("venue_max_ns", p.venue.max);
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(true, "order_bench");
}
