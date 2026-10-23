// Tests for include/fixgw/pipeline.hpp. Spec: docs/design/005-engine.md (D4):
// the three-stage pipeline with a real journal (pre-filled, fdatasync) carries
// every order out and every ExecutionReport back to the application, in
// order; one fdatasync covers several inputs.
#include "fix_text.hpp"
#include "venue.hpp"

#include <fixgw/pipeline.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

using namespace fixgw;
using fixgw::test::check;
using namespace fixgw::test;

void pipeline_round_trips(const std::string& dir, bool drop_midway) {
    tcp_listener l;
    l.open(0);
    journal_file file;
    file.create(dir + (drop_midway ? "/drop.journal" : "/pipeline.journal"), 1, 4u << 20);
    yes y;
    auto in = std::make_unique<app_ring>(), out = std::make_unique<app_ring>();
    engine_config ec;
    ec.port = l.port();
    order_config oc;
    oc.stale_ns = 60'000'000'000;  // reconnection, not D5: a slow reconnect (backoff) must not make orders stale
    pipeline_engine<yes> p(client(), ec, file, y, *in, *out, {}, oc);
    std::atomic<bool> stop{false};
    std::thread t([&] { p.run(stop); });

    tcp_connection vc;
    venue_sink vs;
    vs.c = &vc;
    venue_store vst;
    session<venue_sink, venue_store> v(venue(), vs, vst);
    std::string inbox;
    const auto accept_one = [&] {
        for (int k = 0; k < 20'000'000; ++k) {
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
    int submitted = 0, received = 0, out_of_order = 0, reconnects = 0, dropped_at = drop_midway ? n / 2 : -1;
    const std::int64_t give_up = clock_now().mono_ns + 20'000'000'000;
    while (ok && received < n && clock_now().mono_ns < give_up) {
        char b[4096];
        long r;
        while ((r = vc.read_some(b, sizeof(b))) > 0) inbox.append(b, static_cast<std::size_t>(r));
        if (r < 0) {  // our side closed: take its new connection
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
        if (v.state() == session_state::active && p.connected() && submitted < n && submitted - received < 64 &&
            submit_order(*in, "D", test::soh(test::order_fields("O" + std::to_string(submitted)))))
            ++submitted;
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
    journal_reader rd;
    rd.open(dir + (drop_midway ? "/drop.journal" : "/pipeline.journal"));
    const std::size_t records = rd.records().size();
    check(ok && received == n && out_of_order == 0 && (!drop_midway || reconnects == 1),
          std::string("D4 pipeline") + (drop_midway ? ", connection dropped halfway" : "") + ": " + std::to_string(received) +
              " of " + std::to_string(n) + " ExecutionReports reached the application, in order" +
              (drop_midway ? ", after " + std::to_string(reconnects) + " reconnection" : ""));
    if (!drop_midway)
        check(p.syncs() > 0 && p.syncs() < records,
              "D4 pipeline: " + std::to_string(records) + " records made durable by " + std::to_string(p.syncs()) +
                  " fdatasync calls (several inputs per call)");
}

}  // namespace

int main() {
    char t[] = "pipeline_test_XXXXXX";
    const char* d = ::mkdtemp(t);
    if (!d) {
        check(false, "temporary directory");
        return test::result();
    }
    pipeline_round_trips(d, false);
    pipeline_round_trips(d, true);
    std::filesystem::remove_all(d);
    return test::result();
}
