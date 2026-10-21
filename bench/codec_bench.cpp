// The codec's three representations, measured (note 002, S8): decoding and
// encoding, per call, and decoding throughput over coalesced messages; and the
// run JSON, through lockfree-ipc's harness. Design: docs/design/002-codec.md
//
// Usage: codec_bench [output.json]
// Default output: results/codec-<host>-<UTC time>.json, refused unless the
// machine is in bench mode and the build is clean (lockfree-ipc note 004).
// Method as lockfree-ipc's record cost: each call timed between start() and
// stop(), unsubtracted, beside an empty window ("none") for the clock's own
// cost; CPU 2; 1,024 different messages visited in a random order.
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
#include <cstring>
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
constexpr std::int64_t base_ms = 1'792'567'800'000;  // 2026-10-21 07:30:00 UTC

template <typename T>
inline void keep(const T& v) {
    asm volatile("" : : "g"(&v) : "memory");
}

// Every measured operation is called through its own non-inlined entry point,
// as a caller (the session) would call it. Inlined into the timing loop, the
// same decoder's cost moved by 1.5x with unrelated code elsewhere in the file
// (note 009, Review); through an entry point it is stable. One call's cost
// (~1-2 ns) is included.
using view_t = fx::message_view<>;
[[gnu::noinline]] fx::frame_result op_frame_scalar(const char* p, std::size_t n) { return fx::frame_scalar(p, n, 4096); }
[[gnu::noinline]] fx::frame_result op_frame_simd(const char* p, std::size_t n) { return fx::frame_simd(p, n, 4096); }
[[gnu::noinline]] fx::decode_result op_decode_scalar(const char* p, std::size_t n, view_t& v) {
    return fx::decoder<256, fx::scan::scalar>::decode(p, fx::frame_scalar(p, n, 4096).length, v);
}
[[gnu::noinline]] fx::decode_result op_decode_simd(const char* p, std::size_t n, view_t& v) {
    return fx::decoder<256, fx::scan::simd<false>>::decode(p, fx::frame_simd(p, n, 4096).length, v);
}
[[gnu::noinline]] fx::decode_result op_decode_simd_padded(const char* p, std::size_t n, view_t& v) {
    return fx::decoder<256, fx::scan::simd<true>>::decode(p, fx::frame_simd(p, n, 4096).length, v);
}
// Two-stage on one message: the index built per message, inside the call.
[[gnu::noinline]] fx::decode_result op_decode_index(const char* p, std::size_t n, view_t& v) {
    static fx::structural_index<64> ix;
    const std::size_t len = fx::frame_simd(p, n, 4096).length;
    ix.build<true>(p, len);
    return fx::decode_indexed(p, ix, 0, len, v);
}
[[gnu::noinline]] bool op_decode_a(const char* p, std::size_t n, std::unordered_map<std::uint32_t, std::string>& m) {
    return fx::bench::decode_a(p, fx::frame_scalar(p, n, 4096).length, m);
}
// Decoding alone, for the bursts (framing is the same for every path there).
[[gnu::noinline]] fx::decode_result op_only_scalar(const char* p, std::size_t n, view_t& v) {
    return fx::decoder<256, fx::scan::scalar>::decode(p, n, v);
}
[[gnu::noinline]] fx::decode_result op_only_simd(const char* p, std::size_t n, view_t& v) {
    return fx::decoder<256, fx::scan::simd<false>>::decode(p, n, v);
}
[[gnu::noinline]] fx::decode_result op_only_simd_padded(const char* p, std::size_t n, view_t& v) {
    return fx::decoder<256, fx::scan::simd<true>>::decode(p, n, v);
}
template <std::size_t W>
[[gnu::noinline]] fx::decode_result op_only_indexed(const char* base, const fx::structural_index<W>& ix,
                                                    std::size_t off, std::size_t n, view_t& v) {
    return fx::decode_indexed(base, ix, off, n, v);
}
template <std::size_t W>
[[gnu::noinline]] bool op_index(fx::structural_index<W>& ix, const char* p, std::size_t n) {
    return ix.template build<true>(p, n);
}

// The order inputs are visited in: random, fixed by its seed, read outside the
// timed window. Cycling through the inputs in a fixed order let the branch
// predictor memorise the cycle, case by case, giving a fast mode real traffic
// never sees (note 009, Review).
const std::vector<std::uint32_t>& visit_order() {
    static const std::vector<std::uint32_t> order = [] {
        std::vector<std::uint32_t> o(warmup + samples);
        std::mt19937_64 rng(3);
        for (auto& x : o) x = static_cast<std::uint32_t>(rng() % inputs);
        return o;
    }();
    return order;
}

// Per-call cost of op(j), j in the random visit order.
template <typename Op>
overhead time_op(Op&& op) {
    std::vector<ticks> deltas(samples);
    const auto& order = visit_order();
    for (std::size_t i = 0; i < warmup + samples; ++i) {
        const std::size_t j = order[i];
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
[[gnu::noinline]] std::string_view encode_b(char* buf, std::size_t cap, const order_values& o) {
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

[[gnu::noinline]] std::string encode_a(const order_values& o) {
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

[[gnu::noinline]] std::string_view op_encode_c(fx::new_order_template& t, const order_values& o) {
    return t.fill(o.seq, o.ms, o.clordid, o.qty, o.price);
}

// Decoding inputs with the variety of real order flow (note 009, Review):
// uniform messages (one symbol, fixed-width IDs and prices) made every branch
// outcome repeat, so a predictor in a clean state predicted nearly all of
// them, and each case ran fast or slow depending on the predictor's history.
// Seeded, so every run decodes the same messages.
struct variety {
    std::mt19937_64 rng;
    explicit variety(std::uint64_t seed) : rng(seed) {}
    std::size_t between(std::size_t lo, std::size_t hi) { return lo + rng() % (hi - lo + 1); }
    bool chance(unsigned percent) { return rng() % 100 < percent; }
    std::string text(std::size_t lo, std::size_t hi, const char* alphabet) {
        const std::size_t n = between(lo, hi), k = std::strlen(alphabet);
        std::string out(n, ' ');
        for (auto& c : out) c = alphabet[rng() % k];
        return out;
    }
    std::string symbol() { return text(1, 4, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") + (chance(50) ? ".L" : ""); }  // 1-6
    std::string id(std::size_t lo, std::size_t hi) { return text(lo, hi, "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ-"); }
    std::uint64_t digits(std::size_t lo, std::size_t hi) {  // a number of lo..hi digits
        const std::size_t n = between(lo, hi);
        std::uint64_t lo_v = 1;
        for (std::size_t i = 1; i < n; ++i) lo_v *= 10;
        return lo_v + rng() % (lo_v * 9);
    }
    void price(fx::encoder& e, std::uint32_t tag) {  // 1-5 integer digits, 0-4 decimals
        const int dec = static_cast<int>(between(0, 4));
        std::uint64_t scale = 1;
        for (int i = 0; i < dec; ++i) scale *= 10;
        e.add_decimal(tag, static_cast<std::int64_t>(digits(1, 5) * scale + rng() % scale), dec);
    }
    void timestamp(fx::encoder& e, std::uint32_t tag, std::int64_t ms) {
        char ts[fx::utc_timestamp_length];
        fx::format_utc_timestamp(ms, ts);
        e.add(tag, std::string_view(ts, sizeof(ts)));
    }
};

std::string varied_new_order(variety& v, std::uint64_t seq, std::int64_t ms) {
    char buf[512];
    fx::encoder e(buf, sizeof(buf));
    e.begin("D");
    e.add(49, std::string_view("CLIENT"));
    e.add(56, std::string_view("VENUE"));
    e.add(34, seq);
    v.timestamp(e, 52, ms);
    e.add(11, v.id(6, 20));                       // ClOrdID 6-20
    if (v.chance(80)) e.add(1, v.id(3, 12));      // Account 80% of orders
    e.add(55, v.symbol());
    e.add(54, v.chance(50) ? '1' : '2');
    v.timestamp(e, 60, ms);
    e.add(38, v.digits(1, 7));                    // OrderQty 1-7 digits
    const bool limit = v.chance(85);
    e.add(40, limit ? '2' : '1');
    if (limit) v.price(e, 44);                    // Price on limit orders
    if (v.chance(70)) e.add(59, v.chance(80) ? '0' : '3');  // TimeInForce 70%
    if (v.chance(10)) e.add(58, v.text(5, 40, "abcdefghijklmnopqrstuvwxyz "));  // Text 10%
    return std::string(e.finish());
}

std::string varied_execution_report(variety& v, std::uint64_t seq, std::int64_t ms) {
    char buf[512];
    fx::encoder e(buf, sizeof(buf));
    e.begin("8");
    e.add(49, std::string_view("VENUE"));
    e.add(56, std::string_view("CLIENT"));
    e.add(34, seq);
    v.timestamp(e, 52, ms);
    e.add(37, v.id(8, 24));                       // OrderID
    e.add(11, v.id(6, 20));                       // ClOrdID
    e.add(17, v.id(8, 24));                       // ExecID
    const bool fill = v.chance(60);
    e.add(150, fill ? 'F' : '0');
    e.add(39, fill ? (v.chance(70) ? '2' : '1') : '0');
    if (v.chance(80)) e.add(1, v.id(3, 12));
    e.add(55, v.symbol());
    e.add(54, v.chance(50) ? '1' : '2');
    e.add(38, v.digits(1, 7));
    v.price(e, 44);
    if (fill) {
        e.add(32, v.digits(1, 7));
        v.price(e, 31);
    }
    e.add(151, v.digits(1, 7));
    e.add(14, v.digits(1, 7));
    v.price(e, 6);
    v.timestamp(e, 60, ms);
    if (v.chance(10)) e.add(58, v.text(5, 40, "abcdefghijklmnopqrstuvwxyz "));
    return std::string(e.finish());
}

std::string varied_heartbeat(variety& v, std::uint64_t seq, std::int64_t ms) {
    char buf[256];
    fx::encoder e(buf, sizeof(buf));
    e.begin("0");
    e.add(49, std::string_view("VENUE"));
    e.add(56, std::string_view("CLIENT"));
    e.add(34, seq);
    v.timestamp(e, 52, ms);
    if (v.chance(20)) e.add(112, v.id(4, 16));    // TestReqID, answering a TestRequest
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
    variety vary(9);
    char scratch[512];
    for (std::size_t i = 0; i < inputs; ++i) {
        nos_s[i] = varied_new_order(vary, orders[i].seq, orders[i].ms);
        er_s[i] = varied_execution_report(vary, orders[i].seq, orders[i].ms);
        hb_s[i] = varied_heartbeat(vary, orders[i].seq, orders[i].ms);
    }
    const slots nos = make_slots(nos_s), er = make_slots(er_s), hb = make_slots(hb_s);
    std::vector<result> out;
    fx::message_view<> view;
    std::unordered_map<std::uint32_t, std::string> map;
    map.reserve(64);

    out.push_back({"none", "-", "empty window: start(); stop()", "-", 0, time_op([](std::size_t) {})});
    // Frame and decode, per path.
    const auto path = [&](auto op) {
        return [&, op](const slots& m) {
            return [&, op](std::size_t j) {
                const auto d = op(m.at(j), m.length[j], view);
                keep(d);
                keep(view);
            };
        };
    };
    const auto scalar = path(op_decode_scalar), simd = path(op_decode_simd), simd_padded = path(op_decode_simd_padded),
               indexed = path(op_decode_index);
    const auto rep_a = [&](const slots& m) {
        return [&](std::size_t j) {
            const bool ok = op_decode_a(m.at(j), m.length[j], map);
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
                           const auto f = op_frame_scalar(msg.m->at(j), msg.m->length[j]);
                           keep(f);
                       })});
        out.push_back({"frame_simd_" + t, "SIMD", "frame", msg.name, bytes, time_op([&](std::size_t j) {
                           const auto f = op_frame_simd(msg.m->at(j), msg.m->length[j]);
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
                       const auto s = op_encode_c(tmpl, orders[j]);
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
    // The burst is a random sequence of the inputs, so it does not repeat with
    // a short period either.
    std::vector<std::uint32_t> burst_order(stream_messages);
    {
        std::mt19937_64 rng(4);
        for (auto& x : burst_order) x = static_cast<std::uint32_t>(rng() % inputs);
    }
    for (std::size_t i = 0; i < stream_messages; ++i)
        stream.insert(stream.end(), nos_s[burst_order[i]].begin(), nos_s[burst_order[i]].end());
    const std::size_t stream_bytes = stream.size();
    stream.resize(stream_bytes + 64, '\x7f');
    constexpr std::size_t batch_bytes = 16 * 1024;
    std::vector<std::size_t> batch_end;  // where each batch ends: a message boundary
    {
        std::size_t at = 0, start_of_batch = 0;
        for (std::size_t i = 0; i < stream_messages; ++i) {
            const std::size_t len = nos_s[burst_order[i]].size();
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
                if (index_first) op_index(*batch_ix, stream.data() + base, end - base);
                while (at < end) {
                    const auto f = op_frame_simd(stream.data() + at, end - at);
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
    burst("scalar", [&](std::size_t, std::size_t at, std::size_t len) { return op_only_scalar(stream.data() + at, len, view); }, false);
    burst("SIMD", [&](std::size_t, std::size_t at, std::size_t len) { return op_only_simd(stream.data() + at, len, view); }, false);
    burst("SIMD, padded", [&](std::size_t, std::size_t at, std::size_t len) {
        return op_only_simd_padded(stream.data() + at, len, view); }, false);
    burst("two-stage", [&](std::size_t base, std::size_t at, std::size_t len) {
        return op_only_indexed(stream.data() + base, *batch_ix, at - base, len, view); }, true);

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
