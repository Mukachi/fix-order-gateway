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

void pipeline_round_trips(const std::string& dir) {
    tcp_listener l;
    l.open(0);
    const int c = connect_start(l.port());
    int a = -1;
    for (int k = 0; k < 2'000'000 && a < 0; ++k) a = l.accept();
    for (int k = 0; k < 2'000'000 && connect_check(c) == connect_status::in_progress; ++k) {
    }
    journal_file file;
    file.create(dir + "/pipeline.journal", 1, 4u << 20);
    yes y;
    auto in = std::make_unique<app_ring>(), out = std::make_unique<app_ring>();
    pipeline_engine<yes> p(client(), file, y, *in, *out, c);
    tcp_connection vc;
    vc.attach(a);
    venue_sink vs;
    vs.c = &vc;
    venue_store vst;
    session<venue_sink, venue_store> v(venue(), vs, vst);
    v.on_connected(clock_now());
    std::atomic<bool> stop{false};
    std::thread t([&] { p.run(stop); });

    constexpr int n = 2000;
    std::string inbox;
    int submitted = 0, received = 0, out_of_order = 0;
    const std::int64_t give_up = clock_now().mono_ns + 20'000'000'000;
    while (received < n && clock_now().mono_ns < give_up) {
        char b[4096];
        long r;
        while ((r = vc.read_some(b, sizeof(b))) > 0) inbox.append(b, static_cast<std::size_t>(r));
        if (!inbox.empty()) inbox.erase(0, v.on_bytes(inbox.data(), inbox.size(), clock_now()));
        for (const auto& id : vs.to_answer)
            v.send("8", test::soh("37=X-" + id + "|11=" + id + "|17=E-" + id + "|150=0|39=0|"), clock_now());
        vs.to_answer.clear();
        vc.flush();
        if (v.state() == session_state::active && submitted < n && submitted - received < 64 &&
            submit_order(*in, "D", test::soh("11=O" + std::to_string(submitted) + "|55=VOD.L|54=1|38=100|40=1|")))
            ++submitted;
        app_slot s;
        while (out->try_pop(s)) {
            if (s.kind != app_slot::message) continue;
            const std::string m(s.data, s.length);
            if (m.find("\x01" "35=8\x01") == std::string::npos) continue;
            if (m.find("\x01" "11=O" + std::to_string(received) + "\x01") == std::string::npos) ++out_of_order;
            ++received;
        }
    }
    stop = true;
    t.join();
    journal_reader rd;
    rd.open(dir + "/pipeline.journal");
    const std::size_t records = rd.records().size();
    check(received == n && out_of_order == 0,
          "D4 pipeline: " + std::to_string(received) + " of " + std::to_string(n) + " ExecutionReports reached the application, in order");
    check(p.syncs() > 0 && p.syncs() < records,
          "D4 pipeline: " + std::to_string(records) + " records made durable by " + std::to_string(p.syncs()) +
              " fdatasync calls (several inputs per call)");
    ::close(a);
}

}  // namespace

int main() {
    char t[] = "pipeline_test_XXXXXX";
    const char* d = ::mkdtemp(t);
    if (!d) {
        check(false, "temporary directory");
        return test::result();
    }
    pipeline_round_trips(d);
    std::filesystem::remove_all(d);
    return test::result();
}
