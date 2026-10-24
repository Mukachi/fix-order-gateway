// The gateway's own cost per message, by stage, in process (note 008, D6, S3):
// each call timed between start() and stop(), unsubtracted, beside an empty
// window ("none") for the clock's own cost, on CPU 2, over the same 100,000
// different messages (stage_inputs.hpp) after 10,000 of warm-up, visited in
// order (MsgSeqNum and ExecIDs must advance). What a stage needs but does not
// own (decoding, for the checks that read a decoded message) is done outside
// its window. Stages:
//   ExecutionReport in: framing and decoding; the dictionary check (note 006)
//   alone; the order book (note 007) alone; the session's whole inbound path;
//   the journaled session's (book and journal C, its file on tmpfs).
//   NewOrderSingle out: the body checks; the book's check and record; the
//   session's send (encode, store); the journaled session's send (checks,
//   book, encode, journal C).
// Each stage's branch mispredictions per message, counted over its whole loop
// (the counter read before and after it, nothing added inside: what the loop
// does outside the window counted too, so the dictionary check's includes the
// decoding before it), for note 008's open spread between runs.
// QuickFIX's parse of the same ExecutionReports: qf_bench parse. Run JSON
// through lockfree-ipc's harness. Design: docs/design/008-measurement.md
//
// Usage: stage_bench [output.json]
#include "branch_misses.hpp"
#include "filesystem.hpp"
#include "stage_inputs.hpp"

#include <fixgw/durability.hpp>
#include <fixgw/framing.hpp>
#include <fixgw/orders.hpp>
#include <fixgw/recovery.hpp>
#include <fixgw/session.hpp>
#include <fixgw/validate.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/overhead.hpp>
#include <lfipc/bench/run_file.hpp>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace lfipc::bench;
namespace fx = fixgw;

constexpr int cpu = 2;

struct quiet_sink {
    std::size_t delivered = 0;
    void write(std::string_view) {}
    void disconnect() {}
    void deliver(const fx::message_view<>&) { ++delivered; }
    void log(fx::session_log, std::string_view, std::uint64_t) {}
    bool resend(const fx::message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};
struct quiet_store {
    void record(std::uint64_t, std::string_view) {}
    std::string_view lookup(std::uint64_t) { return {}; }
    void reset() {}
};
struct quiet_out {
    void write(std::string_view) {}
    void deliver(std::string_view, bool) {}
    void order_update(const fx::order_event&, bool) {}
    void disconnect() {}
};
struct quiet_policy {
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

// The time the inputs carry, so the session's SendingTime check passes.
fx::session_time when() {
    const auto ms = fx::detail::parse_utc_timestamp("20261024-09:30:00.123");
    return {1'000'000'000, ms.value_or(0)};
}
std::string venue_logon() {
    return stage::framed("35=A\x01" "49=VENUE\x01" "56=CLIENT\x01" "34=1\x01" "52=20261024-09:30:00.123\x01" "98=0\x01" "108=30\x01");
}

struct point {
    std::string stage, direction;
    overhead cost{};
    bool ok = true;
    double mispredictions = -1;  // per message, the whole loop; -1: not counted
};

const branch_misses* counter = nullptr;
double last_mispredictions = -1;

// Times f(i) for every input; prepare(i), outside the window, first.
template <class Prepare, class F>
overhead per_call(Prepare&& prepare, F&& f) {
    std::vector<ticks> t;
    t.reserve(stage::count - stage::warmup);
    const std::uint64_t m0 = counter ? counter->read() : 0;
    for (std::size_t i = 0; i < stage::count; ++i) {
        prepare(i);
        const ticks a = start();
        f(i);
        const ticks b = stop();
        if (i >= stage::warmup) t.push_back(b - a);
    }
    last_mispredictions = counter ? static_cast<double>(counter->read() - m0) / static_cast<double>(stage::count) : -1;
    overhead o = summarise_overhead(std::move(t));
    o.warmup_discarded = stage::warmup;
    o.cpu = cpu;
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    run_file run(argc, argv, LFIPC_RESULTS_DIR, "stage");
    if (run.refused) return run.finish(false, "stage_bench");
    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "stage_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "stage_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    const branch_misses bm;
    if (bm.ok()) counter = &bm;
    else std::fprintf(stderr, "stage_bench: branch mispredictions not counted (perf_event_paranoid above 2?)\n");
    const auto reports = stage::reports();
    const auto bodies = stage::order_bodies();
    const fx::session_time now = when();
    std::vector<point> points;
    const auto add = [&](point p) {
        p.mispredictions = last_mispredictions;
        points.push_back(std::move(p));
    };
    bool ok = true;
    std::string fs_c = "unknown";  // journal C's file system (tmpfs: note 004's amendment)

    add({"none", "-", per_call([](std::size_t) {}, [](std::size_t) {})});
    {
        fx::message_view<> v;
        bool all = true;
        add({"framing and decoding", "ExecutionReport in", per_call([](std::size_t) {}, [&](std::size_t i) {
                              const auto f = fx::frame(reports[i].data(), reports[i].size(), 65536);
                              all &= f.status == fx::frame_status::complete && fx::decode(reports[i].data(), f.length, v).ok;
                          })});
        points.back().ok = all;
    }
    {
        fx::message_view<> v;
        bool all = true;
        add({"dictionary check (note 006)", "ExecutionReport in",
                          per_call([&](std::size_t i) { fx::decode(reports[i].data(), reports[i].size(), v); },
                                   [&](std::size_t) { all &= fx::validate(v, "8").ok && fx::conditional_rules(v, "8").ok; })});
        points.back().ok = all;
    }
    {
        auto b = std::make_unique<fx::order_book>(book());
        for (std::size_t i = 0; i < stage::count; ++i) b->record("D", bodies[i], now);
        fx::message_view<> v;
        fx::order_event e;
        bool all = true;
        add({"order book (note 007)", "ExecutionReport in",
                          per_call([&](std::size_t i) { fx::decode(reports[i].data(), reports[i].size(), v); },
                                   [&](std::size_t) { all &= b->inbound(v, e) && !e.inconsistent; })});
        points.back().ok = all;
    }
    {
        quiet_sink k;
        quiet_store st;
        fx::session<quiet_sink, quiet_store> s(client(), k, st);
        s.on_connected(now);
        const std::string logon = venue_logon();
        s.on_bytes(logon.data(), logon.size(), now);
        add({"session, the whole inbound path", "ExecutionReport in",
                          per_call([](std::size_t) {}, [&](std::size_t i) { s.on_bytes(reports[i].data(), reports[i].size(), now); })});
        points.back().ok = k.delivered == stage::count;
    }
    {
        char tmpl[64];
        std::snprintf(tmpl, sizeof(tmpl), "%s", bench::journal_c_template);
        const char* d = ::mkdtemp(tmpl);
        if (d) fs_c = bench::filesystem_of(d);
        fx::mapped_journal j;
        const std::string path = std::string(d ? d : ".") + "/in.journal";
        ok &= j.create(path, 1, 512u << 20);
        quiet_out out;
        quiet_policy p;
        auto us = std::make_unique<fx::journaled_session<fx::mapped_journal, quiet_out, quiet_policy>>(client(), j, out, p, book());
        us->on_connected(now);
        const std::string logon = venue_logon();
        us->on_bytes(logon.data(), logon.size(), now);
        for (std::size_t i = 0; i < stage::count; ++i) us->send("D", bodies[i], now);  // the orders the reports answer
        add({"journaled session (book, journal C), inbound", "ExecutionReport in",
                          per_call([](std::size_t) {}, [&](std::size_t i) { us->on_bytes(reports[i].data(), reports[i].size(), now); })});
        points.back().ok = us->orders().orders() == stage::count && us->orders().exec_ids() == stage::count;
        if (d) std::filesystem::remove_all(d);
    }
    {
        bool all = true;
        add({"body checks (note 007)", "NewOrderSingle out", per_call([](std::size_t) {}, [&](std::size_t i) {
                              all &= fx::validate_body("D", bodies[i]).ok && fx::conditional_rules_body("D", bodies[i]).ok;
                          })});
        points.back().ok = all;
    }
    {
        auto b = std::make_unique<fx::order_book>(book());
        bool all = true;
        add({"order book check and record (note 007)", "NewOrderSingle out", per_call([](std::size_t) {}, [&](std::size_t i) {
                              const auto v = b->check("D", bodies[i], now);
                              all &= v.ok;
                              if (v.ok) b->record("D", bodies[i], now);
                          })});
        points.back().ok = all;
    }
    {
        quiet_sink k;
        quiet_store st;
        fx::session<quiet_sink, quiet_store> s(client(), k, st);
        s.on_connected(now);
        const std::string logon = venue_logon();
        s.on_bytes(logon.data(), logon.size(), now);
        bool all = true;
        add({"session send (encode, store)", "NewOrderSingle out", per_call([](std::size_t) {}, [&](std::size_t i) {
                              all &= s.send("D", bodies[i], now) == fx::send_result::sent;
                          })});
        points.back().ok = all;
    }
    {
        char tmpl[64];
        std::snprintf(tmpl, sizeof(tmpl), "%s", bench::journal_c_template);
        const char* d = ::mkdtemp(tmpl);
        if (d) fs_c = bench::filesystem_of(d);
        fx::mapped_journal j;
        const std::string path = std::string(d ? d : ".") + "/out.journal";
        ok &= j.create(path, 1, 512u << 20);
        quiet_out out;
        quiet_policy p;
        auto us = std::make_unique<fx::journaled_session<fx::mapped_journal, quiet_out, quiet_policy>>(client(), j, out, p, book());
        us->on_connected(now);
        const std::string logon = venue_logon();
        us->on_bytes(logon.data(), logon.size(), now);
        bool all = true;
        add({"journaled session send (checks, book, encode, journal C)", "NewOrderSingle out",
                          per_call([](std::size_t) {}, [&](std::size_t i) { all &= us->send("D", bodies[i], now) == fx::send_result::sent; })});
        points.back().ok = all;
        if (d) std::filesystem::remove_all(d);
    }

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto ns = [&](ticks t) { return static_cast<double>(t) / tpn; };
    run.w.key("stage");
    run.w.begin_object();
    run.w.member("gateway", std::string("ours"));
    run.w.member("cpu", cpu);
    run.w.member("messages", stage::count - stage::warmup);
    run.w.member("journal_c_filesystem", fs_c);
    run.w.member("lockfree_ipc_commit", FIXGW_LFIPC_COMMIT);
    run.w.key("points");
    run.w.begin_array();
    for (const auto& p : points) {
        ok &= p.ok;
        run.w.begin_object();
        run.w.member("stage", p.stage);
        run.w.member("direction", p.direction);
        run.w.member("all_correct", p.ok);
        run.w.member("samples", p.cost.samples);
        run.w.member("warmup_discarded", p.cost.warmup_discarded);
        run.w.member("p50_ns", ns(p.cost.p50));
        run.w.member("p99_ns", ns(p.cost.p99));
        run.w.member("p999_ns", ns(p.cost.p999));
        run.w.member("max_ns", ns(p.cost.max));
        run.w.member("mispredictions_per_message", p.mispredictions);
        run.w.end_object();
        std::printf("%-58s %-20s %s p50 %8.1f p99 %8.1f p99.9 %8.1f max %9.1f ns  mispredictions %.2f\n", p.stage.c_str(), p.direction.c_str(),
                    p.ok ? "  " : "!!", ns(p.cost.p50), ns(p.cost.p99), ns(p.cost.p999), ns(p.cost.max), p.mispredictions);
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "stage_bench");
}
