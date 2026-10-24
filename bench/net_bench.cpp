// The engine's measurements (note 005, S5), against net_peer:
//   round trip: a 200-byte message out and back over TCP, per wait mode
//     (busy-poll, block in epoll, spin then block), closed loop: the floor;
//   order path: orders submitted by the application at a fixed rate (open
//     loop) through the engine, the direct loop (strategy A) or the
//     pipeline, to net_peer's venue and its ExecutionReport back to the
//     application's ring; latency from each order's scheduled time.
// Run JSON through lockfree-ipc's harness. Design: docs/design/005-engine.md
//
// Usage: net_bench <peer address> <port> <path name> [output.json]
//   path name: "loopback" or "card" (recorded; the address decides the path)
// net_peer must be serving 3 + 4 connections: 3 echo, then 4 venue.
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
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>

namespace {

using namespace lfipc::bench;
namespace fx = fixgw;

constexpr int app_cpu = 2, engine_cpu = 4, journal_cpu = 6, sender_cpu = 8;
constexpr std::size_t rt_samples = 100'000, rt_warmup = 10'000, echo_size = 200;
constexpr std::size_t order_samples = 20'000, order_warmup = 1'000;

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

int connect_blocking(std::uint32_t ip, std::uint16_t port) {
    const int fd = fx::connect_start(port, {}, ip);
    if (fd < 0) return -1;
    fx::connect_status s;
    while ((s = fx::connect_check(fd)) == fx::connect_status::in_progress) {
    }
    if (s != fx::connect_status::connected) {
        ::close(fd);
        return -1;
    }
    return fd;
}

struct result {
    result(std::string n, std::string l, std::string w, double r = 0)
        : name(std::move(n)), loop(std::move(l)), wait(std::move(w)), rate(r) {}
    std::string name, loop, wait;
    double rate = 0;  // order path: offered per second; round trip: 0
    overhead cost;
    double sustained = 0;
    std::uint64_t syncs = 0, records = 0;
    bool complete = false;
};

// One round-trip point: closed loop, `rt_warmup + rt_samples` messages.
result round_trip(std::uint32_t ip, std::uint16_t port, fx::wait_mode mode) {
    result r("round trip", "-", mode == fx::wait_mode::busy ? "busy-poll" : mode == fx::wait_mode::block ? "block" : "spin then block");
    const int fd = connect_blocking(ip, port);
    if (fd < 0) return r;
    const int ep = ::epoll_create1(EPOLL_CLOEXEC);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    ::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
    char msg[echo_size], back[echo_size];
    for (std::size_t i = 0; i < echo_size; ++i) msg[i] = static_cast<char>('a' + i % 26);
    std::vector<ticks> d(rt_samples);
    bool ok = true;
    for (std::size_t i = 0; ok && i < rt_warmup + rt_samples; ++i) {
        const ticks a = start();
        if (::send(fd, msg, sizeof(msg), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(msg))) ok = false;
        std::size_t got = 0;
        const ticks spin_until = a + 50'000 * 2;  // ~50 us at ~2 ticks/ns
        while (ok && got < sizeof(back)) {
            if (mode == fx::wait_mode::block || (mode == fx::wait_mode::spin_then_block && start() > spin_until)) {
                epoll_event out;
                ::epoll_wait(ep, &out, 1, 1000);
            }
            const ssize_t n = ::recv(fd, back + got, sizeof(back) - got, MSG_DONTWAIT);
            if (n > 0) got += static_cast<std::size_t>(n);
            else if (n == 0) ok = false;
        }
        const ticks b = stop();
        if (i >= rt_warmup) d[i - rt_warmup] = b - a;
    }
    ::close(ep);
    ::close(fd);
    r.cost = summarise_overhead(std::move(d));
    r.cost.warmup_discarded = rt_warmup;
    r.cost.cpu = app_cpu;
    r.complete = ok;
    return r;
}

// The application's side of an order-path point: submit at `rate`, collect
// the ExecutionReports, latency from each order's scheduled time.
template <class Connected>
void drive(fx::app_ring& in, fx::app_ring& out, double rate, double tpn, Connected connected, result& r) {
    const std::size_t n = order_warmup + order_samples;
    std::vector<ticks> due(n, 0), back(n, 0);
    for (int k = 0; k < 5'000'000 && !connected(); ++k) {
    }
    // Wait for the logon: an order that comes back means the session is up.
    const auto collect = [&](std::size_t& received) {
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
    std::string body;
    std::size_t received = 0;
    const ticks interval = static_cast<ticks>(1e9 / rate * tpn);
    ticks t0 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (i == 0) {  // the first order waits for the session; the schedule starts when it is back
            body = "11=0\x01" "55=VOD.L\x01" "54=1\x01" "60=20261024-00:00:00.000\x01" "38=100\x01" "40=2\x01" "44=72.53\x01" "59=0\x01";
            due[0] = start();
            while (!fx::submit_order(in, "D", body)) {
            }
            for (int k = 0; k < 200'000'000 && received == 0; ++k) collect(received);
            t0 = start() + interval;
            continue;
        }
        due[i] = t0 + interval * (i - 1);
        while (start() < due[i]) collect(received);
        body = "11=" + std::to_string(i) + "\x01" "55=VOD.L\x01" "54=1\x01" "60=20261024-00:00:00.000\x01" "38=100\x01" "40=2\x01" "44=72.53\x01" "59=0\x01";
        while (!fx::submit_order(in, "D", body)) collect(received);
    }
    const ticks give_up = start() + static_cast<ticks>(5e9 * tpn);
    while (received < n && start() < give_up) collect(received);
    std::vector<ticks> lat;
    lat.reserve(order_samples);
    ticks last = 0;
    for (std::size_t i = order_warmup; i < n; ++i) {
        lat.push_back(back[i] > due[i] ? back[i] - due[i] : 0);
        if (back[i] > last) last = back[i];
    }
    r.cost = summarise_overhead(std::move(lat));
    r.cost.warmup_discarded = order_warmup;
    r.cost.cpu = app_cpu;
    r.complete = received == n;
    if (r.complete && last > due[order_warmup])
        r.sustained = static_cast<double>(order_samples) / (static_cast<double>(last - due[order_warmup]) / tpn / 1e9);
}

result order_path_direct(std::uint32_t ip, std::uint16_t port, double rate, double tpn, const std::string& dir) {
    result r("order path", "direct", "busy-poll", rate);
    fx::fsync_each j;
    const std::string path = dir + "/direct-" + std::to_string(static_cast<int>(rate)) + ".journal";
    if (!j.create(path, 1, 32u << 20)) return r;
    quiet q;
    auto in = std::make_unique<fx::app_ring>(), out = std::make_unique<fx::app_ring>();
    fx::engine_config ec;
    ec.wait = fx::wait_mode::busy;
    ec.port = port;
    ec.ip = ip;
    fx::engine<fx::fsync_each, quiet> e(client(), ec, j, q, *in, *out);
    std::atomic<bool> stop_flag{false};
    std::thread t([&] {
        detail::pin_to(engine_cpu);
        e.run(stop_flag);
    });
    drive(*in, *out, rate, tpn, [&] { return e.connected(); }, r);
    stop_flag = true;
    t.join();
    fx::journal_reader rd;
    rd.open(path);
    r.records = rd.records().size();
    r.syncs = r.records;  // one fdatasync per input
    return r;
}

result order_path_pipeline(std::uint32_t ip, std::uint16_t port, double rate, double tpn, const std::string& dir) {
    result r("order path", "pipeline", "busy-poll", rate);
    fx::journal_file f;
    const std::string path = dir + "/pipeline-" + std::to_string(static_cast<int>(rate)) + ".journal";
    if (!f.create(path, 1, 32u << 20)) return r;
    quiet q;
    auto in = std::make_unique<fx::app_ring>(), out = std::make_unique<fx::app_ring>();
    fx::engine_config ec;
    ec.port = port;
    ec.ip = ip;
    fx::pipeline_engine<quiet> p(client(), ec, f, q, *in, *out, {engine_cpu, journal_cpu, sender_cpu});
    std::atomic<bool> stop_flag{false};
    std::thread t([&] { p.run(stop_flag); });
    drive(*in, *out, rate, tpn, [&] { return p.connected(); }, r);
    stop_flag = true;
    t.join();
    fx::journal_reader rd;
    rd.open(path);
    r.records = rd.records().size();
    r.syncs = p.syncs();
    return r;
}

// The conditions the kernel's network path ran under: busy polling's defaults
// (D2) and the card's receive interrupt coalescing (-1 if not readable).
long read_sysctl(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (std::fscanf(f, "%ld", &v) != 1) v = -1;
    std::fclose(f);
    return v;
}
long rx_usecs(const char* ifname) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    ethtool_coalesce c{};
    c.cmd = ETHTOOL_GCOALESCE;
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_data = reinterpret_cast<char*>(&c);
    const long v = ::ioctl(fd, SIOCETHTOOL, &ifr) == 0 ? static_cast<long>(c.rx_coalesce_usecs) : -1;
    ::close(fd);
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: net_bench <peer address> <port> <loopback|card> [output.json]\n");
        return 2;
    }
    in_addr a{};
    if (::inet_pton(AF_INET, argv[1], &a) != 1) return 2;
    const std::uint32_t ip = ntohl(a.s_addr);
    const auto port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    const std::string path_name = argv[3];
    // The harness reads argv[1] as the output path: give it ours.
    char* run_argv[] = {argv[0], argc > 4 ? argv[4] : nullptr, nullptr};
    run_file run(argc > 4 ? 2 : 1, run_argv, LFIPC_RESULTS_DIR, "net");
    if (run.refused) return run.finish(false, "net_bench");
    const auto noise = sample_noise({app_cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !detail::pin_to(app_cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "net_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "net_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    char tmpl[] = "net_bench_journals_XXXXXX";
    const char* dir_c = ::mkdtemp(tmpl);
    const std::string dir = dir_c ? std::filesystem::absolute(dir_c).string() : ".";
    std::vector<result> results;
    for (const auto m : {fx::wait_mode::busy, fx::wait_mode::block, fx::wait_mode::spin_then_block})
        results.push_back(round_trip(ip, port, m));
    for (const double rate : {1'000.0, 10'000.0}) {
        results.push_back(order_path_direct(ip, port, rate, tpn, dir));
        results.push_back(order_path_pipeline(ip, port, rate, tpn, dir));
    }
    if (dir_c) std::filesystem::remove_all(dir);

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto ns = [&](ticks t) { return static_cast<double>(t) / tpn; };
    run.w.key("net");
    run.w.begin_object();
    run.w.member("path", path_name);
    run.w.member("peer", std::string(argv[1]));
    run.w.member("app_cpu", app_cpu);
    run.w.member("engine_cpu", engine_cpu);
    run.w.member("journal_cpu", journal_cpu);
    run.w.member("sender_cpu", sender_cpu);
    run.w.member("echo_bytes", echo_size);
    run.w.member("busy_read_us", read_sysctl("/proc/sys/net/core/busy_read"));
    run.w.member("busy_poll_us", read_sysctl("/proc/sys/net/core/busy_poll"));
    run.w.member("card_rx_usecs", rx_usecs("ens1f0"));
    run.w.member("lockfree_ipc_commit", FIXGW_LFIPC_COMMIT);
    run.w.key("points");
    run.w.begin_array();
    for (const auto& r : results) {
        run.w.begin_object();
        run.w.member("measure", r.name);
        run.w.member("loop", r.loop);
        run.w.member("wait", r.wait);
        run.w.member("rate_per_s", r.rate);
        run.w.member("complete", r.complete);
        run.w.member("samples", r.cost.samples);
        run.w.member("warmup_discarded", r.cost.warmup_discarded);
        run.w.member("p50_ns", ns(r.cost.p50));
        run.w.member("p99_ns", ns(r.cost.p99));
        run.w.member("p999_ns", ns(r.cost.p999));
        run.w.member("max_ns", ns(r.cost.max));
        run.w.member("sustained_per_s", r.sustained);
        run.w.member("journal_records", r.records);
        run.w.member("fdatasyncs", r.syncs);
        run.w.end_object();
        std::printf("%-10s %-8s %-15s %6.0f/s %s: p50 %9.1f p99 %9.1f p99.9 %9.1f max %10.1f ns  sustained %7.0f  syncs %llu/%llu\n",
                    r.name.c_str(), r.loop.c_str(), r.wait.c_str(), r.rate, r.complete ? "   " : "INC", ns(r.cost.p50),
                    ns(r.cost.p99), ns(r.cost.p999), ns(r.cost.max), r.sustained, static_cast<unsigned long long>(r.syncs),
                    static_cast<unsigned long long>(r.records));
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(true, "net_bench");
}
