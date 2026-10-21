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
#include <memory>
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

    // Inputs, built before timing. Each message sits at the start of its own
    // 512-byte slot, so every path reads it from the same kind of place and the
    // padded paths have their padding (note 009).
    const auto orders = make_orders();
    struct slots {
        std::vector<char> arena;
        std::vector<std::size_t> length;
        const char* at(std::size_t j) const { return arena.data() + j * 512; }
    };
    const auto make_slots = [](const std::vector<std::string>& ms) {
        slots s;
        s.arena.assign(ms.size() * 512, '\x7f');
        for (std::size_t j = 0; j < ms.size(); ++j) {
            std::copy(ms[j].begin(), ms[j].end(), s.arena.begin() + static_cast<std::ptrdiff_t>(j * 512));
            s.length.push_back(ms[j].size());
        }
        return s;
    };
    std::vector<std::string> nos_s(inputs), er_s(inputs), hb_s(inputs);
    char scratch[512];
    for (std::size_t i = 0; i < inputs; ++i) {
        nos_s[i] = std::string(encode_b(scratch, sizeof(scratch), orders[i]));
        er_s[i] = execution_report(orders[i]);
        hb_s[i] = heartbeat(orders[i]);
    }
    const slots nos = make_slots(nos_s), er = make_slots(er_s), hb = make_slots(hb_s);
    std::vector<result> out;
    fx::message_view<> view;
    std::unordered_map<std::uint32_t, std::string> map;
    map.reserve(64);
    static fx::structural_index<64> small_ix;

    out.push_back({"none", "-", "empty window: start(); stop()", "-", 0, time_op([](std::size_t) {})});
    // Frame and decode, per path.
    const auto scalar = [&](const slots& m) {
        return [&](std::size_t j) {
            const auto f = fx::frame_scalar(m.at(j), m.length[j], 4096);
            const auto d = fx::decoder<256, fx::scan::scalar>::decode(m.at(j), f.length, view);
            keep(d);
            keep(view);
        };
    };
    const auto simd = [&](const slots& m) {
        return [&](std::size_t j) {
            const auto f = fx::frame_simd(m.at(j), m.length[j], 4096);
            const auto d = fx::decoder<256, fx::scan::simd<false>>::decode(m.at(j), f.length, view);
            keep(d);
            keep(view);
        };
    };
    const auto simd_padded = [&](const slots& m) {
        return [&](std::size_t j) {
            const auto f = fx::frame_simd(m.at(j), m.length[j], 4096);
            const auto d = fx::decoder<256, fx::scan::simd<true>>::decode(m.at(j), f.length, view);
            keep(d);
            keep(view);
        };
    };
    // Two-stage on one message: the index is built per message, inside the window.
    const auto indexed = [&](const slots& m) {
        return [&](std::size_t j) {
            const auto f = fx::frame_simd(m.at(j), m.length[j], 4096);
            small_ix.build<true>(m.at(j), f.length);
            const auto d = fx::decode_indexed(m.at(j), small_ix, 0, f.length, view);
            keep(d);
            keep(view);
        };
    };
    const auto rep_a = [&](const slots& m) {
        return [&](std::size_t j) {
            const auto f = fx::frame_scalar(m.at(j), m.length[j], 4096);
            const bool ok = fx::bench::decode_a(m.at(j), f.length, map);
            keep(ok);
            keep(map);
        };
    };
    const struct {
        const char* tag;
        const char* name;
        const slots* m;
    } messages[] = {{"nos", "NewOrderSingle", &nos}, {"er", "ExecutionReport", &er}, {"hb", "Heartbeat", &hb}};
    for (const auto& msg : messages) {
        const std::size_t bytes = msg.m->length[0];
        const std::string t = msg.tag;
        out.push_back({"frame_scalar_" + t, "scalar", "frame", msg.name, bytes, time_op([&](std::size_t j) {
                           const auto f = fx::frame_scalar(msg.m->at(j), msg.m->length[j], 4096);
                           keep(f);
                       })});
        out.push_back({"frame_simd_" + t, "SIMD", "frame", msg.name, bytes, time_op([&](std::size_t j) {
                           const auto f = fx::frame_simd(msg.m->at(j), msg.m->length[j], 4096);
                           keep(f);
                       })});
        out.push_back({"decode_a_" + t, "A", "frame and decode", msg.name, bytes, time_op(rep_a(*msg.m))});
        out.push_back({"decode_scalar_" + t, "scalar", "frame and decode", msg.name, bytes, time_op(scalar(*msg.m))});
        out.push_back({"decode_simd_" + t, "SIMD", "frame and decode", msg.name, bytes, time_op(simd(*msg.m))});
        out.push_back({"decode_simd_padded_" + t, "SIMD, padded", "frame and decode", msg.name, bytes,
                       time_op(simd_padded(*msg.m))});
        out.push_back({"decode_index_" + t, "two-stage", "frame and decode", msg.name, bytes, time_op(indexed(*msg.m))});
    }

    out.push_back({"encode_a_nos", "A", "encode", "NewOrderSingle", nos.length[0], time_op([&](std::size_t j) {
                       const std::string s = encode_a(orders[j]);
                       keep(s);
                   })});
    out.push_back({"encode_b_nos", "B", "encode", "NewOrderSingle", nos.length[0], time_op([&](std::size_t j) {
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

    // Throughput: frame and decode 100,000 coalesced NewOrderSingles, 21
    // times per path, in batches of whole messages up to 16 KB: what one
    // socket read hands the decoder. Two-stage builds its index over each
    // batch, inside the timing (indexing the whole 16 MB at once would measure
    // the memory system, not the method). The stream has 64 bytes of padding
    // past its end.
    constexpr std::size_t stream_messages = 100'000;
    std::vector<char> stream;
    for (std::size_t i = 0; i < stream_messages; ++i) stream.insert(stream.end(), nos_s[i % inputs].begin(), nos_s[i % inputs].end());
    const std::size_t stream_bytes = stream.size();
    stream.resize(stream_bytes + 64, '\x7f');
    constexpr std::size_t batch_bytes = 16 * 1024;
    std::vector<std::size_t> batch_end;  // where each batch ends: a message boundary
    {
        std::size_t at = 0, start_of_batch = 0;
        for (std::size_t i = 0; i < stream_messages; ++i) {
            const std::size_t len = nos_s[i % inputs].size();
            if (at + len - start_of_batch > batch_bytes) {
                batch_end.push_back(at);
                start_of_batch = at;
            }
            at += len;
        }
        batch_end.push_back(at);
    }
    auto batch_ix = std::make_unique<fx::structural_index<batch_bytes / 64 + 1>>();
    struct path_throughput {
        const char* name;
        std::vector<double> per_second;
    };
    std::vector<path_throughput> through;
    const auto burst = [&](const char* name, auto&& decode_one, bool index_first) {
        path_throughput t{name, {}};
        for (int rep = 0; rep < 21; ++rep) {
            const ticks a = start();
            std::size_t at = 0, n = 0;
            for (const std::size_t end : batch_end) {
                const std::size_t base = at;
                if (index_first) batch_ix->build<true>(stream.data() + base, end - base);
                while (at < end) {
                    const auto f = fx::frame_simd(stream.data() + at, end - at, 4096);
                    const auto d = decode_one(base, at, f.length);
                    keep(d);
                    at += f.length;
                    ++n;
                }
            }
            const ticks b = stop();
            if (n == stream_messages)
                t.per_second.push_back(static_cast<double>(n) * clk.cal.ticks_per_ns * 1e9 / static_cast<double>(b - a));
        }
        std::sort(t.per_second.begin(), t.per_second.end());
        through.push_back(std::move(t));
    };
    burst("scalar", [&](std::size_t, std::size_t at, std::size_t len) {
        return fx::decoder<256, fx::scan::scalar>::decode(stream.data() + at, len, view); }, false);
    burst("SIMD", [&](std::size_t, std::size_t at, std::size_t len) {
        return fx::decoder<256, fx::scan::simd<false>>::decode(stream.data() + at, len, view); }, false);
    burst("SIMD, padded", [&](std::size_t, std::size_t at, std::size_t len) {
        return fx::decoder<256, fx::scan::simd<true>>::decode(stream.data() + at, len, view); }, false);
    burst("two-stage", [&](std::size_t base, std::size_t at, std::size_t len) {
        return fx::decode_indexed(stream.data() + base, *batch_ix, at - base, len, view); }, true);

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
        std::printf("%-26s %-13s %-17s %-16s %4zu B: p50 %6.1f p99 %6.1f p99.9 %6.1f max %8.1f ns\n", r.name.c_str(),
                    r.representation.c_str(), r.operation.c_str(), r.message.c_str(), r.bytes, ns(r.cost.p50),
                    ns(r.cost.p99), ns(r.cost.p999), ns(r.cost.max));
    }
    run.w.end_object();
    run.w.key("throughput");
    run.w.begin_object();
    run.w.member("operation", "frame and decode, NewOrderSingle, coalesced");
    run.w.member("messages", stream_messages);
    run.w.member("batch_bytes", batch_bytes);
    run.w.key("paths");
    run.w.begin_object();
    for (const auto& t : through) {
        run.w.key(t.name);
        run.w.begin_object();
        run.w.member("repeats", t.per_second.size());
        if (!t.per_second.empty()) {
            run.w.member("min_per_s", t.per_second.front());
            run.w.member("p50_per_s", t.per_second[t.per_second.size() / 2]);
            run.w.member("max_per_s", t.per_second.back());
            std::printf("throughput %-13s p50 %.2fM messages/s (min %.2fM, max %.2fM)\n", t.name,
                        t.per_second[t.per_second.size() / 2] / 1e6, t.per_second.front() / 1e6,
                        t.per_second.back() / 1e6);
        }
        run.w.end_object();
    }
    run.w.end_object();  // paths
    run.w.end_object();  // throughput
    run.w.end_object();  // codec
    return run.finish(true, "codec_bench");
}
