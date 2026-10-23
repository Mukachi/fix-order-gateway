// The counterparty for net_bench (note 005, S5): listens, serves one
// connection at a time on its own pinned CPU, busy-polling. A connection whose
// first byte is '8' (a FIX message) gets a venue: a plain acceptor session
// that answers each order with an ExecutionReport naming it. Any other gets an
// echo of fixed-size messages. Exits after `connections` connections.
// Design: docs/design/005-engine.md
//
// Usage: net_peer <listen address> <port> <cpu> <connections> [echo size]
#include <fixgw/engine.hpp>
#include <fixgw/session.hpp>
#include <fixgw/transport.hpp>

#include <lfipc/bench/cross_cpu.hpp>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <arpa/inet.h>

namespace {

using namespace fixgw;

struct venue_sink {
    tcp_connection* c;
    std::vector<std::string> to_answer;
    void write(std::string_view b) { c->write(b); }
    void disconnect() { c->close(); }
    void deliver(const message_view<>& m) { to_answer.emplace_back(m.get(11).value_or("")); }
    void log(session_log, std::string_view, std::uint64_t) {}
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};
struct venue_store {
    std::map<std::uint64_t, std::string> m;
    void record(std::uint64_t s, std::string_view b) { m[s] = std::string(b); }
    std::string_view lookup(std::uint64_t s) {
        const auto it = m.find(s);
        return it == m.end() ? std::string_view{} : std::string_view(it->second);
    }
    void reset() { m.clear(); }
};

void serve_venue(tcp_connection& c, std::string inbox) {
    session_config cfg;
    cfg.role = session_role::acceptor;
    cfg.sender_comp_id = "VENUE";
    cfg.target_comp_id = "CLIENT";
    venue_sink s{&c, {}};
    venue_store st;
    session<venue_sink, venue_store> v(cfg, s, st);
    v.on_connected(clock_now());
    char b[16 << 10];
    std::string body;
    while (c.open()) {
        const session_time now = clock_now();
        if (!inbox.empty()) inbox.erase(0, v.on_bytes(inbox.data(), inbox.size(), now));
        for (const auto& id : s.to_answer) {
            body = "37=X" + id + "\x01" "11=" + id + "\x01" "17=E" + id + "\x01" "150=0\x01" "39=0\x01" "55=VOD.L\x01" "54=1\x01";
            v.send("8", body, now);
        }
        s.to_answer.clear();
        c.flush();
        v.on_tick(now);
        const long n = c.read_some(b, sizeof(b));
        if (n < 0) break;
        if (n > 0) inbox.append(b, static_cast<std::size_t>(n));
    }
}

void serve_echo(tcp_connection& c, std::string inbox, std::size_t size) {
    char b[16 << 10];
    while (c.open()) {
        while (inbox.size() >= size) {
            c.write(std::string_view(inbox.data(), size));
            inbox.erase(0, size);
        }
        c.flush();
        const long n = c.read_some(b, sizeof(b));
        if (n < 0) break;
        if (n > 0) inbox.append(b, static_cast<std::size_t>(n));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: net_peer <listen address> <port> <cpu> <connections> [echo size]\n");
        return 2;
    }
    in_addr a{};
    if (::inet_pton(AF_INET, argv[1], &a) != 1) return 2;
    const auto port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    const int cpu = std::atoi(argv[3]), connections = std::atoi(argv[4]);
    const std::size_t echo_size = argc > 5 ? static_cast<std::size_t>(std::atoi(argv[5])) : 200;
    if (!lfipc::bench::detail::pin_to(cpu)) std::fprintf(stderr, "net_peer: cannot pin to CPU %d\n", cpu);
    tcp_listener l;
    if (!l.open(port, ntohl(a.s_addr))) {
        std::fprintf(stderr, "net_peer: cannot listen on %s:%u\n", argv[1], port);
        return 1;
    }
    std::fprintf(stderr, "net_peer: listening on %s:%u, CPU %d, %d connections\n", argv[1], port, cpu, connections);
    for (int k = 0; k < connections; ++k) {
        int fd = -1;
        while ((fd = l.accept()) < 0) {
        }
        tcp_connection c;
        c.attach(fd);
        std::string first;
        char b[4096];
        while (first.empty()) {
            const long n = c.read_some(b, sizeof(b));
            if (n < 0) break;
            if (n > 0) first.assign(b, static_cast<std::size_t>(n));
        }
        if (first.empty()) continue;
        if (first[0] == '8') serve_venue(c, first);
        else serve_echo(c, first, echo_size);
    }
    return 0;
}
