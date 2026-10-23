// The journal's cost on the send path (note 004, S5): for each durability
// strategy, the time from an order's scheduled arrival to the release of its
// bytes, open loop at fixed rates; group commit at three bounds; replay speed.
// Run JSON through lockfree-ipc's harness. Design: docs/design/004-store.md
//
// Usage: store_bench [output.json]
// Default output: results/store-<host>-<UTC time>.json, refused unless the
// machine is in bench mode and the build is clean (lockfree-ipc note 004).
// The journals are written under ./store_bench_journals_XXXXXX (the current
// directory's filesystem, recorded) and removed at the end.
#include <fixgw/durability.hpp>
#include <fixgw/encode.hpp>
#include <fixgw/recovery.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/overhead.hpp>
#include <lfipc/bench/run_file.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace lfipc::bench;
namespace fx = fixgw;

constexpr int cpu = 2, standby_cpu = 4;
constexpr std::size_t samples = 20'000, warmup = 1'000;
constexpr std::int64_t base_utc_ms = 1'792'567'800'000;  // 2026-10-21 07:30:00 UTC

// Where each released order's bytes went out, by order.
struct release_out {
    std::vector<ticks>* released;
    std::size_t next = 0;
    bool dropped = false;
    void write(std::string_view b) {
        const ticks t = stop();
        for (std::size_t at = b.find("\x01" "35=D\x01"); at != std::string_view::npos; at = b.find("\x01" "35=D\x01", at + 1))
            if (next < released->size()) (*released)[next++] = t;
    }
    void deliver(std::string_view, bool) {}
    void disconnect() { dropped = true; }
};
struct quiet {
    void log(fx::session_log, std::string_view, std::uint64_t) {}
    bool resend(const fx::message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};

fx::session_config config() {
    fx::session_config c;
    c.role = fx::session_role::initiator;
    c.sender_comp_id = "CLIENT";
    c.target_comp_id = "VENUE";
    return c;
}

struct point_result {
    std::string strategy, bound;
    double rate;
    overhead cost;
    std::uint64_t bytes_written = 0;
    bool complete = false;
    double sustained_per_s = 0;  // orders released per second, first scheduled to last released
};

std::uint64_t file_size(const std::string& p) {
    struct stat s;
    return ::stat(p.c_str(), &s) == 0 ? static_cast<std::uint64_t>(s.st_size) : 0;
}

// One point: a logged-on session over strategy `j`, `warmup + samples`
// orders scheduled `rate` a second apart; each one's latency is from its
// scheduled time to its release.
template <class J>
point_result measure(J& j, const std::string& journal, double ticks_per_ns, double rate, std::string strategy,
                     std::string bound) {
    const std::size_t n = warmup + samples;
    std::vector<ticks> released(n, 0), arrival(n, 0);
    release_out out{&released};
    quiet q;
    fx::journaled_session<J, release_out, quiet> us(config(), j, out, q);
    const auto time_at = [&](ticks t) {
        const auto ns = static_cast<std::int64_t>(static_cast<double>(t) / ticks_per_ns);
        return fx::session_time{ns, base_utc_ms + ns / 1'000'000};
    };
    // Log on: our Logon, the counterparty's reply, settled.
    us.on_connected(time_at(start()));
    {
        char buf[512], ts[fx::utc_timestamp_length];
        const auto now = time_at(start());
        fx::format_utc_timestamp(now.utc_ms, ts);
        fx::encoder e(buf, sizeof(buf));
        e.begin("A");
        e.add(49, "VENUE");
        e.add(56, "CLIENT");
        e.add(34, std::uint64_t{1});
        e.add(52, std::string_view(ts, sizeof(ts)));
        e.add(98, std::uint64_t{0});
        e.add(108, std::uint64_t{30});
        const auto m = e.finish();
        us.on_bytes(m.data(), m.size(), now);
    }
    for (int k = 0; k < 1000 && us.holding(); ++k) us.poll(time_at(start() + static_cast<ticks>(1e9 * ticks_per_ns)));
    if (us.state().state() != fx::session_state::active) return {strategy, bound, rate, {}, 0, false};
    out.next = 0;
    // The orders, built before timing: ClOrdID changes, so does the length.
    std::vector<std::string> bodies(n);
    for (std::size_t i = 0; i < n; ++i)
        bodies[i] = "11=ORD" + std::to_string(1'000'000 + i) + "\x01" "1=ACC1\x01" "55=VOD.L\x01" "54=1\x01" "38=" +
                    std::to_string(100 * (1 + i % 50)) + "\x01" "40=2\x01" "44=72.53\x01" "59=0\x01";
    const std::uint64_t bytes_before = file_size(journal);
    const ticks interval = static_cast<ticks>(1e9 / rate * ticks_per_ns), t0 = start() + interval;
    for (std::size_t i = 0; i < n; ++i) {
        const ticks due = t0 + interval * i;
        ticks t;
        while ((t = start()) < due) us.poll(time_at(t));
        arrival[i] = due;
        us.send("D", bodies[i], time_at(t));
        if (out.dropped) return {strategy, bound, rate, {}, 0, false};
    }
    for (int k = 0; k < 100'000 && us.holding(); ++k) us.poll(time_at(start()));
    std::vector<ticks> lat;
    lat.reserve(samples);
    for (std::size_t i = warmup; i < n; ++i) lat.push_back(released[i] >= arrival[i] ? released[i] - arrival[i] : 0);
    point_result r{strategy, bound, rate, summarise_overhead(std::move(lat)), file_size(journal) - bytes_before,
                   out.next == n, 0};
    if (r.complete && released[n - 1] > arrival[0])
        r.sustained_per_s = static_cast<double>(n) / (static_cast<double>(released[n - 1] - arrival[0]) / ticks_per_ns / 1e9);
    r.cost.warmup_discarded = warmup;
    r.cost.cpu = cpu;
    return r;
}

std::string filesystem_of(const std::string& dir) {
    struct statfs s;
    if (::statfs(dir.c_str(), &s) != 0) return "unknown";
    switch (static_cast<unsigned long>(s.f_type)) {
        case 0xEF53: return "ext4";
        case 0x01021994: return "tmpfs";
        case 0x58465342: return "xfs";
        case 0x9123683E: return "btrfs";
        default: return "other";
    }
}

}  // namespace

int main(int argc, char** argv) {
    run_file run(argc, argv, LFIPC_RESULTS_DIR, "store");
    if (run.refused) return run.finish(false, "store_bench");
    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "store_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "store_bench");
    }
    char tmpl[] = "store_bench_journals_XXXXXX";
    const char* dir_c = ::mkdtemp(tmpl);
    if (!dir_c) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "store_bench: refused: cannot create the journal directory\n");
        return run.finish(false, "store_bench");
    }
    const std::string dir = std::filesystem::absolute(dir_c).string();
    const std::string fs = filesystem_of(dir);
    const double tpn = clk.cal.ticks_per_ns;
    std::vector<point_result> points;
    int file_no = 0;
    const auto path = [&] { return dir + "/" + std::to_string(++file_no) + ".journal"; };

    for (const double rate : {1'000.0, 5'000.0, 20'000.0}) {
        {
            fx::fsync_each j;
            const auto p = path();
            j.create(p, 1);
            points.push_back(measure(j, p, tpn, rate, "A", "-"));
        }
        for (const auto [records, wait_us] : {std::pair{4, 20}, std::pair{16, 100}, std::pair{64, 500}}) {
            fx::group_commit j(static_cast<std::size_t>(records), static_cast<std::int64_t>(wait_us) * 1000);
            const auto p = path();
            j.create(p, 1);
            points.push_back(measure(j, p, tpn, rate, "B",
                                     std::to_string(records) + " records or " + std::to_string(wait_us) + " us"));
        }
        {
            fx::mapped_journal j;
            const auto p = path();
            j.create(p, 1, 256u << 20);
            points.push_back(measure(j, p, tpn, rate, "C", "-"));
            points.back().bytes_written = 0;  // the file is its capacity; what was written is not its size
        }
        {
            // D: the standby in this process, on its own core, over loopback.
            const int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            socklen_t len = sizeof(a);
            ::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
            ::listen(lfd, 1);
            ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &len);
            const std::string standby_path = path();
            std::thread standby([lfd, standby_path] {
                detail::pin_to(standby_cpu);
                const int c = ::accept(lfd, nullptr, nullptr);
                fx::journal_file f;
                f.create(standby_path, 1);
                fx::run_standby(c, f);
                ::close(c);
            });
            {
                fx::replicated j;
                const auto p = path();
                j.create(p, 1);
                j.connect_standby(ntohs(a.sin_port));
                points.push_back(measure(j, p, tpn, rate, "D", "standby on CPU 4, loopback"));
            }
            standby.join();
            ::close(lfd);
        }
    }

    // Replay: a journal of a million order records, read and recovered.
    constexpr std::size_t replay_records = 1'000'000;
    double replay_s = 0;
    std::uint64_t replay_bytes = 0;
    std::size_t replayed = 0;
    {
        const auto p = path();
        {
            fx::journal_file f;
            f.create(p, 1);
            const std::string body = "11=ORD1234567\x01" "1=ACC1\x01" "55=VOD.L\x01" "54=1\x01" "38=100\x01" "40=2\x01" "44=72.53\x01" "59=0\x01";
            // A logon pair first, so the orders replay into a logged-on session.
            f.append(fx::record_kind::connected, {0, base_utc_ms}, "");
            char buf[512], ts[fx::utc_timestamp_length];
            fx::format_utc_timestamp(base_utc_ms, ts);
            fx::encoder e(buf, sizeof(buf));
            e.begin("A");
            e.add(49, "VENUE");
            e.add(56, "CLIENT");
            e.add(34, std::uint64_t{1});
            e.add(52, std::string_view(ts, sizeof(ts)));
            e.add(98, std::uint64_t{0});
            e.add(108, std::uint64_t{30});
            const auto m = e.finish();
            f.append(fx::record_kind::bytes, {1, base_utc_ms}, m);
            for (std::size_t i = 0; i < replay_records; ++i)
                f.append(fx::record_kind::send, {static_cast<std::int64_t>(2 + i), base_utc_ms + 1}, "\x01" "D", body);
        }
        replay_bytes = file_size(p);
        const ticks a = start();
        fx::journal_reader r;
        bool ok = r.open(p);
        struct none {
            bool append(fx::record_kind, fx::session_time, std::string_view, std::string_view) { return false; }
            fx::durability make_durable(fx::session_time) { return fx::durability::failed; }
            fx::durability poll(fx::session_time) { return fx::durability::durable; }
        } nj;
        std::vector<ticks> sink(1);
        release_out out{&sink};
        quiet q;
        auto again = std::make_unique<fx::journaled_session<none, release_out, quiet>>(config(), nj, out, q);
        ok = ok && again->recover(r);
        const ticks b = stop();
        replay_s = static_cast<double>(b - a) / tpn / 1e9;
        replayed = ok ? r.records().size() : 0;
    }
    std::filesystem::remove_all(dir);

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto ns = [&](ticks t) { return static_cast<double>(t) / tpn; };
    run.w.key("store");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("standby_cpu", standby_cpu);
    run.w.member("journal_filesystem", fs);
    run.w.member("samples", samples);
    run.w.member("warmup_discarded", warmup);
    run.w.member("lockfree_ipc_commit", FIXGW_LFIPC_COMMIT);
    run.w.key("points");
    run.w.begin_array();
    for (const auto& p : points) {
        run.w.begin_object();
        run.w.member("strategy", p.strategy);
        run.w.member("bound", p.bound);
        run.w.member("rate_per_s", p.rate);
        run.w.member("complete", p.complete);
        run.w.member("samples", p.cost.samples);
        run.w.member("p50_ns", ns(p.cost.p50));
        run.w.member("p99_ns", ns(p.cost.p99));
        run.w.member("p999_ns", ns(p.cost.p999));
        run.w.member("max_ns", ns(p.cost.max));
        run.w.member("journal_bytes_written", p.bytes_written);
        run.w.member("sustained_per_s", p.sustained_per_s);
        run.w.end_object();
        std::printf("%-2s %-28s %6.0f/s %s: p50 %10.1f p99 %10.1f p99.9 %10.1f max %11.1f ns, %8llu bytes, sustained %7.0f/s\n",
                    p.strategy.c_str(), p.bound.c_str(), p.rate, p.complete ? "   " : "INC", ns(p.cost.p50),
                    ns(p.cost.p99), ns(p.cost.p999), ns(p.cost.max), static_cast<unsigned long long>(p.bytes_written),
                    p.sustained_per_s);
    }
    run.w.end_array();
    run.w.key("replay");
    run.w.begin_object();
    run.w.member("records", replayed);
    run.w.member("bytes", replay_bytes);
    run.w.member("seconds", replay_s);
    run.w.member("records_per_s", replay_s > 0 ? static_cast<double>(replayed) / replay_s : 0.0);
    run.w.end_object();
    run.w.end_object();
    std::printf("replay: %zu records, %llu bytes, %.3f s, %.0f records/s\n", replayed,
                static_cast<unsigned long long>(replay_bytes), replay_s, replay_s > 0 ? replayed / replay_s : 0.0);
    return run.finish(true, "store_bench");
}
