// Order-to-ExecutionReport latency through QuickFIX v1.16.0 (note 008, D1-D5),
// on the same schedule and against the same venue (order_venue) as
// order_bench: limit NewOrderSingles built before the schedule starts, each
// sent at its scheduled time with Session::sendToTarget, each ExecutionReport
// stamped in fromApp. Written to QuickFIX's documented API, not from its
// source. Configurations, with what each survives (D3):
//   file store, PersistMessages=Y (documented default)  a process crash (it never
//       syncs: 0 fsync, 0 fdatasync observed, note 008's Review)
//   PersistMessages=N                                   nothing persisted
// Its threads: after logon, its process's threads are read from /proc; the
// application thread is pinned to CPU 2 and the others confined together to
// as many further reserved CPUs as there are (D4), the count recorded. No
// message log (ours logs none). Run JSON through lockfree-ipc's harness.
// Design: docs/design/008-measurement.md
//
// Also, as the reference for note 008's D6, QuickFIX's parsing of the same
// ExecutionReports stage_bench measures ours on (stage_inputs.hpp): each
// FIX::Message constructed from the bytes with its FIX 4.4 data dictionary
// validating (the documented constructor), timed per call as stage_bench does.
//
// Usage: qf_bench <venue port> <venue dir> <FIX44.xml> [output.json [--quick]]
//        qf_bench parse <FIX44.xml> [output.json]
#include "stage_inputs.hpp"

#include <quickfix/Application.h>
#include <quickfix/DataDictionary.h>
#include <quickfix/FileStore.h>
#include <quickfix/Session.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketInitiator.h>
#include <quickfix/fix44/NewOrderSingle.h>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/overhead.hpp>
#include <lfipc/bench/run_file.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sched.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>

namespace {

using namespace lfipc::bench;

constexpr int app_cpu = 2;
constexpr int confine_cpus[] = {4, 6, 8, 10, 12, 14, 16, 18};
std::size_t samples = 100'000, warmup = 5'000;

struct venue_figures {
    bool read = false;
    std::size_t samples = 0;
    double p50 = 0, p99 = 0, p999 = 0, max = 0;
};
// The venue's figures for one point: the files it wrote for the connections
// made during it (`before`: the files there when the point started), the one
// with the most orders (a point that fell behind may have reconnected).
venue_figures read_venue(const std::string& dir, const std::vector<std::string>& before) {
    venue_figures v;
    const auto fresh = [&] {
        std::vector<std::string> out;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            const std::string n = e.path().filename().string();
            if (n.starts_with("venue-") && n.ends_with(".txt") && std::find(before.begin(), before.end(), n) == before.end())
                out.push_back(e.path().string());
        }
        return out;
    };
    for (int i = 0; i < 30000 && fresh().empty(); ++i) ::usleep(1000);  // a point that fell behind: the venue drains it first
    ::usleep(200'000);  // a second connection's file, if any
    for (const auto& path : fresh()) {
        std::ifstream f(path);
        std::string w;
        double conn;
        venue_figures c;
        if (f >> w >> conn >> w >> c.samples >> w >> c.p50 >> w >> c.p99 >> w >> c.p999 >> w >> c.max) {
            c.read = c.samples > 0;
            if (c.samples > v.samples) v = c;
        }
    }
    return v;
}
std::vector<std::string> venue_files(const std::string& dir) {
    std::vector<std::string> out;
    for (const auto& e : std::filesystem::directory_iterator(dir)) out.push_back(e.path().filename().string());
    return out;
}

class client : public FIX::Application {
public:
    std::atomic<bool> logged_on{false};
    std::atomic<std::size_t> received{0};
    std::vector<ticks>* back = nullptr;
    FIX::SessionID id;
    void onCreate(const FIX::SessionID& s) override { id = s; }
    void onLogon(const FIX::SessionID&) override { logged_on = true; }
    void onLogout(const FIX::SessionID&) override { logged_on = false; }
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}
    void fromApp(const FIX::Message& m, const FIX::SessionID&) override {
        const ticks t = stop();
        FIX::MsgType type;
        m.getHeader().getField(type);
        if (type.getValue() != "8") return;
        FIX::ClOrdID c;
        m.getField(c);
        const std::size_t i = std::strtoull(c.getValue().c_str(), nullptr, 10);
        if (back && i < back->size() && (*back)[i] == 0) {
            (*back)[i] = t;
            received.fetch_add(1, std::memory_order_release);
        }
    }
};

// Pins this thread to CPU 2 and confines every other thread of the process
// to as many further reserved CPUs as there are of them; returns the count.
int confine_threads() {
    const pid_t self = static_cast<pid_t>(::syscall(SYS_gettid));
    std::vector<pid_t> others;
    if (DIR* d = ::opendir("/proc/self/task")) {
        while (const dirent* e = ::readdir(d))
            if (e->d_name[0] != '.' && std::atoi(e->d_name) != self) others.push_back(std::atoi(e->d_name));
        ::closedir(d);
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    for (std::size_t i = 0; i < others.size() && i < std::size(confine_cpus); ++i) CPU_SET(confine_cpus[i], &set);
    for (const pid_t t : others) ::sched_setaffinity(t, sizeof(set), &set);
    detail::pin_to(app_cpu);
    return static_cast<int>(others.size()) + 1;
}

struct point {
    std::string store, guarantee;
    double rate = 0;
    overhead cost{};
    double sustained = 0;
    bool complete = false;
    int threads = 0;
    venue_figures venue{};
};

point measure(std::uint16_t port, const std::string& dictionary, const std::string& dir, bool persist, double rate, double tpn, int k) {
    point r;
    r.store = persist ? "file store" : "PersistMessages=N";
    r.guarantee = persist ? "process crash" : "nothing persisted";
    r.rate = rate;
    const std::string here = dir + "/qf" + std::to_string(k);
    std::filesystem::create_directories(here);
    {
        std::ofstream f(here + "/settings.cfg");
        f << "[DEFAULT]\nConnectionType=initiator\nSocketConnectHost=127.0.0.1\nSocketConnectPort=" << port
          << "\nReconnectInterval=1\nHeartBtInt=30\nFileStorePath=" << here << "/store\nStartTime=00:00:00\nEndTime=00:00:00\n"
          << "UseDataDictionary=Y\nDataDictionary=" << dictionary << "\nSocketNodelay=Y\nPersistMessages=" << (persist ? "Y" : "N")
          << "\n\n[SESSION]\nBeginString=FIX.4.4\nSenderCompID=CLIENT\nTargetCompID=VENUE\n";
    }
    const std::size_t n = warmup + samples;
    std::vector<ticks> due(n, 0), back(n, 0);
    std::vector<FIX44::NewOrderSingle> orders;
    orders.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        FIX44::NewOrderSingle o(FIX::ClOrdID(std::to_string(i)), FIX::Side(FIX::Side_BUY), FIX::TransactTime(),
                                FIX::OrdType(FIX::OrdType_LIMIT));
        o.set(FIX::HandlInst('1'));
        o.set(FIX::Symbol("VOD.L"));
        o.set(FIX::OrderQty(100));
        o.set(FIX::Price(72.53));
        orders.push_back(std::move(o));
    }
    FIX::SessionSettings settings(here + "/settings.cfg");
    client app;
    app.back = &back;
    FIX::FileStoreFactory store(settings);
    FIX::SocketInitiator initiator(app, store, settings);
    initiator.start();
    for (int i = 0; i < 20'000 && !app.logged_on; ++i) ::usleep(1000);
    r.threads = confine_threads();
    const ticks interval = static_cast<ticks>(1e9 / rate * tpn);
    ticks t0 = 0;
    for (std::size_t i = 0; i < n && app.logged_on; ++i) {
        if (i == 0) {
            due[0] = start();
            FIX::Session::sendToTarget(orders[0], app.id);
            for (int k2 = 0; k2 < 200'000'000 && app.received.load(std::memory_order_acquire) == 0; ++k2) {
            }
            t0 = start() + interval;
            continue;
        }
        due[i] = t0 + interval * (i - 1);
        while (start() < due[i]) {
        }
        FIX::Session::sendToTarget(orders[i], app.id);
    }
    const ticks give_up = start() + static_cast<ticks>(10e9 * tpn);
    while (app.received.load(std::memory_order_acquire) < n && start() < give_up) {
    }
    initiator.stop();
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
    r.complete = app.received.load() == n;
    if (r.complete && last > due[warmup])
        r.sustained = static_cast<double>(samples) / (static_cast<double>(last - due[warmup]) / tpn / 1e9);
    std::filesystem::remove_all(here);
    detail::pin_to(app_cpu);
    return r;
}

// QuickFIX's parse and validation of the same ExecutionReports (D6).
int parse_mode(int argc, char** argv) {
    char* run_argv[] = {argv[0], argc > 3 ? argv[3] : nullptr, nullptr};
    run_file run(argc > 3 ? 2 : 1, run_argv, LFIPC_RESULTS_DIR, "stage_qf");
    if (run.refused) return run.finish(false, "qf_bench parse");
    const auto noise = sample_noise({app_cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !detail::pin_to(app_cpu)) {
        write_clock_block(run.w, clk);
        return run.finish(false, "qf_bench parse");
    }
    const double tpn = clk.cal.ticks_per_ns;
    const FIX::DataDictionary dd(argv[2]);
    const auto reports = stage::reports();
    std::vector<ticks> none, t;
    none.reserve(stage::count);
    t.reserve(stage::count);
    std::size_t valid = 0;
    for (std::size_t i = 0; i < stage::count; ++i) {
        const ticks a = start();
        const ticks b = stop();
        if (i >= stage::warmup) none.push_back(b - a);
    }
    for (std::size_t i = 0; i < stage::count; ++i) {
        const ticks a = start();
        bool ok = true;
        try {
            FIX::Message m(reports[i], dd, true);
        } catch (const std::exception&) {
            ok = false;
        }
        const ticks b = stop();
        valid += ok;
        if (i >= stage::warmup) t.push_back(b - a);
    }
    const overhead on = summarise_overhead(std::move(none)), op = summarise_overhead(std::move(t));
    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto ns = [&](ticks x) { return static_cast<double>(x) / tpn; };
    run.w.key("stage_qf");
    run.w.begin_object();
    run.w.member("gateway", std::string("quickfix"));
    run.w.member("quickfix_version", std::string("1.16.0"));
    run.w.member("cpu", app_cpu);
    run.w.member("messages", stage::count - stage::warmup);
    run.w.member("lockfree_ipc_commit", FIXGW_LFIPC_COMMIT);
    run.w.key("points");
    run.w.begin_array();
    for (const auto& [name, o] : {std::pair<std::string, const overhead*>{"none", &on},
                                  {"FIX::Message from the bytes, the FIX 4.4 dictionary validating", &op}}) {
        run.w.begin_object();
        run.w.member("stage", name);
        run.w.member("direction", std::string(name == "none" ? "-" : "ExecutionReport in"));
        run.w.member("all_correct", valid == stage::count);
        run.w.member("samples", o->samples);
        run.w.member("warmup_discarded", stage::warmup);
        run.w.member("p50_ns", ns(o->p50));
        run.w.member("p99_ns", ns(o->p99));
        run.w.member("p999_ns", ns(o->p999));
        run.w.member("max_ns", ns(o->max));
        run.w.end_object();
        std::printf("%-64s p50 %8.1f p99 %8.1f p99.9 %8.1f max %9.1f ns\n", name.c_str(), ns(o->p50), ns(o->p99), ns(o->p999),
                    ns(o->max));
    }
    run.w.end_array();
    run.w.end_object();
    std::printf("%zu of %zu valid\n", valid, stage::count);
    return run.finish(valid == stage::count, "qf_bench parse");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "parse") return parse_mode(argc, argv);
    if (argc < 4) {
        std::fprintf(stderr, "usage: qf_bench <venue port> <venue dir> <FIX44.xml> [output.json [--quick]]\n");
        return 2;
    }
    const auto port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    const std::string venue_dir = argv[2], dictionary = argv[3];
    if (argc > 5 && std::string(argv[5]) == "--quick") {
        if (std::string(argv[4]).find("/results/") != std::string::npos) {
            std::fprintf(stderr, "qf_bench: --quick is for check runs, not results/\n");
            return 2;
        }
        samples = 10'000;
        warmup = 1'000;
    }
    char* run_argv[] = {argv[0], argc > 4 ? argv[4] : nullptr, nullptr};
    run_file run(argc > 4 ? 2 : 1, run_argv, LFIPC_RESULTS_DIR, "order_qf");
    if (run.refused) return run.finish(false, "qf_bench");
    const auto noise = sample_noise({app_cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !detail::pin_to(app_cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "qf_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "qf_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    char tmpl[] = "qf_bench_stores_XXXXXX";
    const char* dir_c = ::mkdtemp(tmpl);
    const std::string dir = dir_c ? std::filesystem::absolute(dir_c).string() : ".";
    std::vector<point> points;
    int k = 0;
    for (const bool persist : {true, false}) {
        for (const double rate : {1'000.0, 10'000.0, 20'000.0, 50'000.0, 100'000.0}) {
            const auto before = venue_files(venue_dir);
            point p = measure(port, dictionary, dir, persist, rate, tpn, k);
            p.venue = read_venue(venue_dir, before);
            ++k;
            std::printf("qfix  %-18s %7.0f/s %s: p50 %9.1f p99 %9.1f p99.9 %9.1f max %10.1f ns  sustained %8.0f  threads %d  venue p50 %6.0f ns\n",
                        p.store.c_str(), p.rate, p.complete ? "   " : "INC", static_cast<double>(p.cost.p50) / tpn,
                        static_cast<double>(p.cost.p99) / tpn, static_cast<double>(p.cost.p999) / tpn,
                        static_cast<double>(p.cost.max) / tpn, p.sustained, p.threads, p.venue.p50);
            std::fflush(stdout);
            points.push_back(p);
            if (rate > 10'000.0 && !(p.complete && p.sustained >= 0.95 * rate)) break;
        }
    }
    if (dir_c) std::filesystem::remove_all(dir);

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto ns = [&](ticks t) { return static_cast<double>(t) / tpn; };
    run.w.key("order_qf");
    run.w.begin_object();
    run.w.member("gateway", std::string("quickfix"));
    run.w.member("quickfix_version", std::string("1.16.0"));
    run.w.member("path", std::string("loopback"));
    run.w.member("quick", samples != 100'000);
    run.w.member("app_cpu", app_cpu);
    run.w.member("lockfree_ipc_commit", FIXGW_LFIPC_COMMIT);
    run.w.key("points");
    run.w.begin_array();
    for (const auto& p : points) {
        run.w.begin_object();
        run.w.member("gateway", std::string("quickfix"));
        run.w.member("loop", std::string("-"));
        run.w.member("journal", p.store);
        run.w.member("wait", std::string("its own"));
        run.w.member("survives", p.guarantee);
        run.w.member("threads", p.threads);
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
    return run.finish(true, "qf_bench");
}
