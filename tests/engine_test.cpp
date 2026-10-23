// Tests for include/fixgw/engine.hpp. Spec: docs/design/005-engine.md (D1,
// D5, S4): the direct loop on its own thread, in each wait mode; the
// application submits orders through its ring and reads the counterparty's
// ExecutionReports from the other, every one, in order; a dropped connection
// is re-established.
#include "fix_text.hpp"
#include "venue.hpp"

#include <fixgw/engine.hpp>

#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace fixgw;
using fixgw::test::check;
using namespace fixgw::test;

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
            v.send("8", test::soh(test::report_fields(id, "E-" + id)), clock_now());
        vs.to_answer.clear();
        vc.flush();
        v.on_tick(clock_now());
        // the application
        if (v.state() == session_state::active && submitted < n && submitted - received < 64) {
            if (submit_order(*in, "D", test::soh(test::order_fields("O" + std::to_string(submitted))),
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
