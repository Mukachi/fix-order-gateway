// Tests for include/fixgw/engine.hpp. Spec: docs/design/005-engine.md (D1,
// D5, S4): the direct loop on its own thread, in each wait mode; the
// application submits orders through its ring and reads the counterparty's
// ExecutionReports from the other, every one, in order; a dropped connection
// is re-established.
#include "fix_text.hpp"

#include <fixgw/engine.hpp>

#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace fixgw;
using fixgw::test::check;

struct memory_journal {
    bool append(record_kind, session_time, std::string_view, std::string_view) { return true; }
    durability make_durable(session_time) { return durability::durable; }
    durability poll(session_time) { return durability::durable; }
};
struct yes {
    void log(session_log, std::string_view, std::uint64_t) {}
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};

// The counterparty answers each order with an ExecutionReport naming it.
struct venue_sink {
    tcp_connection* c = nullptr;
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

session_config client() {
    session_config c;
    c.role = session_role::initiator;
    c.sender_comp_id = "CLIENT";
    c.target_comp_id = "VENUE";
    return c;
}
session_config venue() {
    session_config c;
    c.role = session_role::acceptor;
    c.sender_comp_id = "VENUE";
    c.target_comp_id = "CLIENT";
    return c;
}

const char* name(wait_mode w) {
    return w == wait_mode::busy ? "busy-poll" : w == wait_mode::block ? "block in epoll" : "spin, then block";
}

void run_mode(wait_mode mode, bool drop_midway) {
    tcp_listener l;
    l.open(0);
    engine_config ec;
    ec.wait = mode;
    ec.port = l.port();
    memory_journal j;
    yes y;
    auto in = std::make_unique<app_ring>(), out = std::make_unique<app_ring>();
    engine<memory_journal, yes> e(client(), ec, j, y, *in, *out);
    std::atomic<bool> stop{false};
    std::thread t([&] { e.run(stop); });

    tcp_connection vc;
    venue_sink vs;
    vs.c = &vc;
    venue_store vst;
    session<venue_sink, venue_store> v(venue(), vs, vst);
    std::string inbox;
    const auto accept_one = [&] {
        for (int k = 0; k < 2'000'000; ++k) {
            const int fd = l.accept();
            if (fd >= 0) {
                vc.attach(fd);
                inbox.clear();
                v.on_connected(clock_now());
                return true;
            }
        }
        return false;
    };
    bool ok = accept_one();
    constexpr int n = 2000;
    int submitted = 0, received = 0, out_of_order = 0, dropped_at = drop_midway ? n / 2 : -1, reconnects = 0;
    const std::int64_t give_up = clock_now().mono_ns + 20'000'000'000;
    while (ok && received < n && clock_now().mono_ns < give_up) {
        // the counterparty
        char b[4096];
        long r;
        while ((r = vc.read_some(b, sizeof(b))) > 0) inbox.append(b, static_cast<std::size_t>(r));
        if (r < 0) {  // our side reconnecting: take the new connection
            v.on_disconnected(clock_now());
            ok = accept_one();
            ++reconnects;
            continue;
        }
        if (!inbox.empty()) inbox.erase(0, v.on_bytes(inbox.data(), inbox.size(), clock_now()));
        for (const auto& id : vs.to_answer)
            v.send("8", test::soh("37=X-" + id + "|11=" + id + "|17=E-" + id + "|150=0|39=0|"), clock_now());
        vs.to_answer.clear();
        vc.flush();
        v.on_tick(clock_now());
        // the application
        if (v.state() == session_state::active && submitted < n && submitted - received < 64) {
            if (submit_order(*in, "D", test::soh("11=O" + std::to_string(submitted) + "|55=VOD.L|54=1|38=100|40=1|"),
                             e.doorbell()))
                ++submitted;
        }
        app_slot s;
        while (out->try_pop(s)) {
            if (s.kind != app_slot::message) continue;
            const std::string m(s.data, s.length);
            if (m.find("\x01" "35=8\x01") == std::string::npos) continue;
            if (m.find("\x01" "11=O" + std::to_string(received) + "\x01") == std::string::npos) ++out_of_order;
            ++received;
        }
        if (dropped_at >= 0 && received >= dropped_at) {  // the counterparty drops the connection once
            vc.close();
            v.on_disconnected(clock_now());
            dropped_at = -1;
            ok = accept_one();
            ++reconnects;
        }
    }
    stop = true;
    t.join();
    check(ok && received == n && out_of_order == 0 && (!drop_midway || reconnects == 1),
          std::string("engine, ") + name(mode) + (drop_midway ? ", connection dropped halfway" : "") + ": " +
              std::to_string(received) + " of " + std::to_string(n) + " ExecutionReports reached the application in order" +
              (drop_midway ? ", after " + std::to_string(reconnects) + " reconnection" : ""));
}

void ring_limits() {
    auto r = std::make_unique<app_ring>();
    check(!submit_order(*r, "D", std::string(505, 'x')) && submit_order(*r, "D", std::string(504, 'x')) &&
              !submit_order(*r, "", "11=A\x01") && !submit_order(*r, "ABCDE", "11=A\x01"),
          "D5: a body over 504 bytes, or a MsgType empty or over 4, is refused at the ring");
}

}  // namespace

int main() {
    ring_limits();
    for (const wait_mode m : {wait_mode::busy, wait_mode::block, wait_mode::spin_then_block}) run_mode(m, false);
    run_mode(wait_mode::busy, true);
    return test::result();
}
