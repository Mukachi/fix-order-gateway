// The venue for order_bench and qf_bench (note 008, D2): a plain acceptor
// session and the venue simulator (note 007) on one pinned CPU, busy-polling,
// one connection at a time. Each NewOrderSingle is answered at once (a limit
// order with nothing to cross rests: ExecutionReport New). What one read
// produces leaves in one send (a send a report costs ~3 us on loopback: behind,
// the venue would never catch up). Its own turnaround
// is timed: from the bytes holding an order being read to the report's bytes
// leaving (CLOCK_MONOTONIC); when a connection ends, its percentiles go to
// <dir>/venue-<n>.txt, read by the benchmark into its run file.
// Design: docs/design/008-measurement.md
//
// Usage: order_venue <port> <cpu> <connections> <dir>
#include <fixgw/engine.hpp>
#include <fixgw/session.hpp>
#include <fixgw/transport.hpp>
#include <fixgw/venue_sim.hpp>

#include <lfipc/bench/cross_cpu.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace fixgw;

struct store {
    std::map<std::uint64_t, std::string> m;
    void record(std::uint64_t s, std::string_view b) { m[s] = std::string(b); }
    std::string_view lookup(std::uint64_t s) {
        const auto it = m.find(s);
        return it == m.end() ? std::string_view{} : std::string_view(it->second);
    }
    void reset() { m.clear(); }
};

struct sink {
    tcp_connection* c = nullptr;
    venue_sim* sim = nullptr;
    session<sink, store>* self = nullptr;
    int orders = 0;   // in this read
    std::string out;  // this read's output, sent once
    void write(std::string_view b) { out += b; }
    void send() {
        if (!out.empty()) c->write(out);
        out.clear();
        c->flush();
    }
    void disconnect() {
        send();  // a Logout before the close
        c->close();
    }
    void deliver(const message_view<>& m) {
        if (m.msg_type() == "D") ++orders;
        sim->on_message(1, m, [&](int s, std::string_view t, const std::string& b) {
            if (s == 1) self->send(t, b, clock_now());
        });
    }
    void log(session_log, std::string_view, std::uint64_t) {}
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};

void write_summary(const std::string& path, std::vector<std::int64_t>& ns, int n) {
    std::sort(ns.begin(), ns.end());
    const auto at = [&](double q) { return ns.empty() ? 0 : ns[std::min(ns.size() - 1, static_cast<std::size_t>(q * static_cast<double>(ns.size())))]; };
    std::ofstream f(path + ".tmp");
    f << "connection " << n << " samples " << ns.size() << " p50_ns " << at(0.5) << " p99_ns " << at(0.99) << " p999_ns "
      << at(0.999) << " max_ns " << (ns.empty() ? 0 : ns.back()) << "\n";
    f.close();
    std::filesystem::rename(path + ".tmp", path);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: order_venue <port> <cpu> <connections> <dir>\n");
        return 2;
    }
    const auto port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    const int cpu = std::atoi(argv[2]), connections = std::atoi(argv[3]);
    const std::string dir = argv[4];
    if (!lfipc::bench::detail::pin_to(cpu)) std::fprintf(stderr, "order_venue: cannot pin to CPU %d\n", cpu);
    tcp_listener l;
    if (!l.open(port)) {
        std::fprintf(stderr, "order_venue: cannot listen on %u\n", port);
        return 1;
    }
    session_config cfg;
    cfg.role = session_role::acceptor;
    cfg.sender_comp_id = "VENUE";
    cfg.target_comp_id = "CLIENT";
    cfg.app_msg_types = "D F G";
    for (int k = 0; k < connections; ++k) {
        int fd;
        while ((fd = l.accept()) < 0) {
        }
        tcp_connection c(64u << 20);
        c.attach(fd);
        venue_config vc;
        vc.capacity = 1u << 18;  // more than a point's orders: no vector grows during it
        venue_sim sim(vc);  // a book per connection: each point starts empty
        store st;
        sink sk;
        sk.c = &c;
        sk.sim = &sim;
        session<sink, store> s(cfg, sk, st);
        sk.self = &s;
        sk.out.reserve(1u << 20);
        s.on_connected(clock_now());
        std::vector<std::int64_t> turnaround;
        turnaround.reserve(1u << 20);
        std::string inbox;
        char b[1 << 16];
        while (c.open()) {
            const long n = c.read_some(b, sizeof(b));
            if (n < 0) break;
            const session_time t0 = clock_now();
            if (n > 0) inbox.append(b, static_cast<std::size_t>(n));
            sk.orders = 0;
            if (!inbox.empty()) inbox.erase(0, s.on_bytes(inbox.data(), inbox.size(), t0));
            sk.send();
            if (sk.orders > 0) {
                const std::int64_t d = clock_now().mono_ns - t0.mono_ns;
                for (int i = 0; i < sk.orders; ++i) turnaround.push_back(d);  // each order of the read: the read's
            }
            s.on_tick(t0);
            sk.send();
        }
        write_summary(dir + "/venue-" + std::to_string(k) + ".txt", turnaround, k);
    }
    return 0;
}
