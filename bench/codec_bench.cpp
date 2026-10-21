// The codec's three representations, measured (note 002, S8): decoding and
// encoding, per call, and decoding throughput over coalesced messages; and the
// run JSON, through lockfree-ipc's harness. Design: docs/design/002-codec.md
//
// Usage: codec_bench [output.json]
// Default output: results/codec-<host>-<UTC time>.json, refused unless the
// machine is in bench mode and the build is clean (lockfree-ipc note 004).
// Method as lockfree-ipc's record cost: each call timed between start() and
// stop(), unsubtracted, beside an empty window ("none") for the clock's own
// cost; CPU 2; inputs cycled through 1,024 different messages.
#include "representation_a.hpp"

#include <fixgw/decode.hpp>
#include <fixgw/encode.hpp>
#include <fixgw/framing.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/overhead.hpp>
#include <lfipc/bench/run_file.hpp>

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace lfipc::bench;
namespace fx = fixgw;

constexpr int cpu = 2;
constexpr std::size_t inputs = 1024, samples = 1'000'000, warmup = 100'000;
constexpr std::int64_t base_ms = 1'792'567'800'000;  // 2026-10-21 09:30:00 UTC

template <typename T>
inline void keep(const T& v) {
    asm volatile("" : : "g"(&v) : "memory");
}

// Per-call cost of op(i), i cycling over the inputs.
template <typename Op>
overhead time_op(Op&& op) {
    std::vector<ticks> deltas(samples);
    for (std::size_t i = 0; i < warmup + samples; ++i) {
        const std::size_t j = i % inputs;
        const ticks a = start();
        op(j);
        const ticks b = stop();
        if (i >= warmup) deltas[i - warmup] = b - a;
    }
    auto o = summarise_overhead(std::move(deltas));
    o.warmup_discarded = warmup;
    o.cpu = cpu;
    return o;
}

struct order_values {
    std::uint64_t seq, clordid, qty, price;
    std::int64_t ms;
};

std::vector<order_values> make_orders() {
    std::mt19937_64 rng(1);
    std::vector<order_values> v(inputs);
    for (std::size_t i = 0; i < inputs; ++i)
        v[i] = {1000 + i, 1'000'000 + rng() % 9'000'000, 100 * (1 + rng() % 500), 7000 + rng() % 1000,
                base_ms + static_cast<std::int64_t>(i)};
    return v;
}

// A NewOrderSingle through encoder B, every field formatted as an order path
// would: the timestamp and ClOrdID from numbers.
std::string_view encode_b(char* buf, std::size_t cap, const order_values& o) {
    char ts[fx::utc_timestamp_length];
    fx::format_utc_timestamp(o.ms, ts);
    const std::string_view t(ts, sizeof(ts));
    fx::encoder e(buf, cap);
    e.begin("D");
    e.add(49, "CLIENT");
    e.add(56, "VENUE");
    e.add(34, o.seq);
    e.add(52, t);
    e.add(11, o.clordid);
    e.add(1, "ACC1");
    e.add(55, "VOD.L");
    e.add(54, '1');
    e.add(60, t);
    e.add(38, o.qty);
    e.add(40, '2');
    e.add_decimal(44, static_cast<std::int64_t>(o.price), 2);
    e.add(59, '0');
    return e.finish();
}

std::string encode_a(const order_values& o) {
    char ts[fx::utc_timestamp_length];
    fx::format_utc_timestamp(o.ms, ts);
    const std::string t(ts, sizeof(ts));
    fx::bench::message_a m("D");
    m.set(49, std::string("CLIENT"));
    m.set(56, std::string("VENUE"));
    m.set(34, o.seq);
    m.set(52, t);
    m.set(11, o.clordid);
    m.set(1, std::string("ACC1"));
    m.set(55, std::string("VOD.L"));
    m.set(54, '1');
    m.set(60, t);
    m.set(38, o.qty);
    m.set(40, '2');
    m.set(44, std::to_string(o.price / 100) + "." + (o.price % 100 < 10 ? "0" : "") + std::to_string(o.price % 100));
    m.set(59, '0');
    return m.serialise();
}

// An ExecutionReport (fill) as a venue would send it.
std::string execution_report(const order_values& o) {
    char ts[fx::utc_timestamp_length];
    fx::format_utc_timestamp(o.ms, ts);
    const std::string_view t(ts, sizeof(ts));
    char buf[512];
    fx::encoder e(buf, sizeof(buf));
    e.begin("8");
    e.add(49, "VENUE");
    e.add(56, "CLIENT");
    e.add(34, o.seq);
    e.add(52, t);
    e.add(37, o.clordid + 5'000'000);
    e.add(11, o.clordid);
    e.add(17, o.clordid + 9'000'000);
    e.add(150, 'F');
    e.add(39, '2');
    e.add(1, "ACC1");
    e.add(55, "VOD.L");
    e.add(54, '1');
    e.add(38, o.qty);
    e.add_decimal(44, static_cast<std::int64_t>(o.price), 2);
    e.add(32, o.qty);
    e.add_decimal(31, static_cast<std::int64_t>(o.price), 2);
    e.add(151, std::uint64_t{0});
    e.add(14, o.qty);
    e.add_decimal(6, static_cast<std::int64_t>(o.price), 2);
    e.add(60, t);
    return std::string(e.finish());
}

std::string heartbeat(const order_values& o) {
    char ts[fx::utc_timestamp_length];
    fx::format_utc_timestamp(o.ms, ts);
    char buf[256];
    fx::encoder e(buf, sizeof(buf));
    e.begin("0");
    e.add(49, "VENUE");
    e.add(56, "CLIENT");
    e.add(34, o.seq);
    e.add(52, std::string_view(ts, sizeof(ts)));
    return std::string(e.finish());
}

struct result {
    std::string name, representation, operation, message;
    std::size_t bytes;
    overhead cost;
};

}  // namespace

int main(int argc, char** argv) {
    run_file run(argc, argv, LFIPC_RESULTS_DIR, "codec");
    if (run.refused) return run.finish(false, "codec_bench");

    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "codec_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "codec_bench");
    }

    // Inputs, built before timing.
    const auto orders = make_orders();
    std::vector<std::string> nos(inputs), er(inputs), hb(inputs);
    char scratch[512];
    for (std::size_t i = 0; i < inputs; ++i) {
        nos[i] = std::string(encode_b(scratch, sizeof(scratch), orders[i]));
        er[i] = execution_report(orders[i]);
        hb[i] = heartbeat(orders[i]);
    }
    std::vector<result> out;
    fx::message_view<> view;
    std::unordered_map<std::uint32_t, std::string> map;
    map.reserve(64);

    out.push_back({"none", "-", "empty window: start(); stop()", "-", 0, time_op([](std::size_t) {})});
    const auto decode_b = [&](const std::vector<std::string>& m) {
        return [&](std::size_t j) {
            const auto f = fx::frame(m[j].data(), m[j].size(), 4096);
            const auto d = fx::decode(m[j].data(), f.length, view);
            keep(d);
            keep(view);
        };
    };
    const auto decode_a = [&](const std::vector<std::string>& m) {
        return [&](std::size_t j) {
            const auto f = fx::frame(m[j].data(), m[j].size(), 4096);
            const bool ok = fx::bench::decode_a(m[j].data(), f.length, map);
            keep(ok);
            keep(map);
        };
    };
    out.push_back({"decode_b_nos", "B", "frame and decode", "NewOrderSingle", nos[0].size(), time_op(decode_b(nos))});
    out.push_back({"decode_a_nos", "A", "frame and decode", "NewOrderSingle", nos[0].size(), time_op(decode_a(nos))});
    out.push_back({"decode_b_er", "B", "frame and decode", "ExecutionReport", er[0].size(), time_op(decode_b(er))});
    out.push_back({"decode_a_er", "A", "frame and decode", "ExecutionReport", er[0].size(), time_op(decode_a(er))});
    out.push_back({"decode_b_hb", "B", "frame and decode", "Heartbeat", hb[0].size(), time_op(decode_b(hb))});

    out.push_back({"encode_a_nos", "A", "encode", "NewOrderSingle", nos[0].size(), time_op([&](std::size_t j) {
                       const std::string s = encode_a(orders[j]);
                       keep(s);
                   })});
    out.push_back({"encode_b_nos", "B", "encode", "NewOrderSingle", nos[0].size(), time_op([&](std::size_t j) {
                       const auto s = encode_b(scratch, sizeof(scratch), orders[j]);
                       keep(s);
                   })});
    fx::new_order_template tmpl({"CLIENT", "VENUE", "ACC1", "VOD.L", '1', '2', '0', 2});
    const std::size_t c_bytes = tmpl.fill(1, base_ms, 1, 1, 1).size();
    out.push_back({"encode_c_nos", "C", "encode", "NewOrderSingle", c_bytes, time_op([&](std::size_t j) {
                       const auto& o = orders[j];
                       const auto s = tmpl.fill(o.seq, o.ms, o.clordid, o.qty, o.price);
                       keep(s);
                   })});

    // Throughput: frame and decode (B) 100,000 coalesced NewOrderSingles, 21 times.
    std::string stream;
    constexpr std::size_t stream_messages = 100'000;
    for (std::size_t i = 0; i < stream_messages; ++i) stream += nos[i % inputs];
    std::vector<double> per_second;
    for (int rep = 0; rep < 21; ++rep) {
        const ticks a = start();
        std::size_t at = 0, n = 0;
        while (at < stream.size()) {
            const auto f = fx::frame(stream.data() + at, stream.size() - at, 4096);
            const auto d = fx::decode(stream.data() + at, f.length, view);
            keep(d);
            at += f.length;
            ++n;
        }
        const ticks b = stop();
        if (n == stream_messages) per_second.push_back(static_cast<double>(n) * clk.cal.ticks_per_ns * 1e9 / static_cast<double>(b - a));
    }
    std::sort(per_second.begin(), per_second.end());

    write_noise_block(run.w, noise, clk.cal.ticks_per_ns);
    write_clock_block(run.w, clk);
    const auto ns = [&](ticks t) { return static_cast<double>(t) / clk.cal.ticks_per_ns; };
    run.w.key("codec");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("inputs", inputs);
    run.w.member("lockfree_ipc_commit", FIXGW_LFIPC_COMMIT);
    run.w.key("cases");
    run.w.begin_object();
    for (const auto& r : out) {
        run.w.key(r.name);
        run.w.begin_object();
        run.w.member("representation", r.representation);
        run.w.member("operation", r.operation);
        run.w.member("message", r.message);
        run.w.member("bytes", r.bytes);
        run.w.member("samples", r.cost.samples);
        run.w.member("warmup_discarded", r.cost.warmup_discarded);
        run.w.member("p50_ns", ns(r.cost.p50));
        run.w.member("p99_ns", ns(r.cost.p99));
        run.w.member("p999_ns", ns(r.cost.p999));
        run.w.member("max_ns", ns(r.cost.max));
        run.w.end_object();
        std::printf("%-14s %s %-17s %-16s %4zu B: p50 %6.1f p99 %6.1f p99.9 %6.1f max %8.1f ns\n", r.name.c_str(),
                    r.representation.c_str(), r.operation.c_str(), r.message.c_str(), r.bytes, ns(r.cost.p50),
                    ns(r.cost.p99), ns(r.cost.p999), ns(r.cost.max));
    }
    run.w.end_object();
    run.w.key("throughput");
    run.w.begin_object();
    run.w.member("operation", "frame and decode (B), NewOrderSingle, coalesced");
    run.w.member("messages", stream_messages);
    run.w.member("repeats", per_second.size());
    if (!per_second.empty()) {
        run.w.member("min_per_s", per_second.front());
        run.w.member("p50_per_s", per_second[per_second.size() / 2]);
        run.w.member("max_per_s", per_second.back());
        std::printf("throughput: frame and decode, p50 %.2fM messages/s (min %.2fM, max %.2fM)\n",
                    per_second[per_second.size() / 2] / 1e6, per_second.front() / 1e6, per_second.back() / 1e6);
    }
    run.w.end_object();
    run.w.end_object();
    return run.finish(true, "codec_bench");
}
