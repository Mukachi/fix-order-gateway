// Note 005, S6: a pass of the loop that moves a message allocates nothing,
// checked as the code runs: every allocation made by any thread but the
// test's own (which plays application and counterparty) is counted, and there
// must be none while orders and ExecutionReports flow in steady state (after
// warm-up). Both loops: the
// direct one, and the pipeline's three stages. Rare paths that may grow a
// buffer (the kernel's send buffer full, the application far behind) are not
// exercised here, and are stated as such.
// The global allocator is replaced below (malloc and free behind new and
// delete, the standard way to count allocations); GCC cannot see that the
// pairs match across the replacement.
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"

#include "fix_text.hpp"
#include "venue.hpp"

#include <fixgw/durability.hpp>
#include <fixgw/engine.hpp>
#include <fixgw/pipeline.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <thread>

#include <unistd.h>

namespace {
std::atomic<bool> counting{false};
thread_local bool test_thread = false;  // the application and counterparty: not counted
std::atomic<long> engine_allocations{0};
}  // namespace

void* operator new(std::size_t n) {
    if (counting.load(std::memory_order_relaxed) && !test_thread) engine_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace fixgw;
using fixgw::test::check;
using namespace fixgw::test;

// The test thread: counterparty and application. `warm` orders round-trip
// before counting starts, then `n` more while it counts.
template <class Connected>
bool exchange(tcp_listener& l, app_ring& in, app_ring& out, Connected connected, int warm, int n) {
    tcp_connection vc;
    venue_sink vs;
    vs.c = &vc;
    venue_store vst;
    session<venue_sink, venue_store> v(venue(), vs, vst);
    for (int k = 0; k < 5'000'000 && !vc.open(); ++k) {
        const int fd = l.accept();
        if (fd >= 0) {
            vc.attach(fd);
            v.on_connected(clock_now());
        }
    }
    std::string inbox;
    int submitted = 0, received = 0;
    const std::int64_t give_up = clock_now().mono_ns + 20'000'000'000;
    while (received < warm + n && clock_now().mono_ns < give_up) {
        if (received >= warm) counting = true;  // >=: one pass may collect several
        char b[4096];
        long r;
        while ((r = vc.read_some(b, sizeof(b))) > 0) inbox.append(b, static_cast<std::size_t>(r));
        if (!inbox.empty()) inbox.erase(0, v.on_bytes(inbox.data(), inbox.size(), clock_now()));
        for (const auto& id : vs.to_answer)
            v.send("8", test::soh("37=X-" + id + "|11=" + id + "|17=E-" + id + "|150=0|39=0|"), clock_now());
        vs.to_answer.clear();
        vc.flush();
        if (connected() && v.state() == session_state::active && submitted < warm + n && submitted - received < 16 &&
            submit_order(in, "D", test::soh("11=O" + std::to_string(submitted) + "|55=VOD.L|54=1|38=100|40=1|")))
            ++submitted;
        app_slot s;
        while (out.try_pop(s))
            if (s.kind == app_slot::message && std::string_view(s.data, s.length).find("\x01" "35=8\x01") != std::string_view::npos)
                ++received;
    }
    counting = false;
    return received == warm + n;
}

void direct(const std::string& dir) {
    tcp_listener l;
    l.open(0);
    fsync_each j;
    j.create(dir + "/direct.journal", 1, 8u << 20);
    yes y;
    auto in = std::make_unique<app_ring>(), out = std::make_unique<app_ring>();
    engine_config ec;
    ec.port = l.port();
    engine<fsync_each, yes> e(client(), ec, j, y, *in, *out);
    std::atomic<bool> stop{false};
    std::thread t([&] { e.run(stop); });
    engine_allocations = 0;
    const bool ok = exchange(l, *in, *out, [&] { return e.connected(); }, 200, 2000);
    stop = true;
    t.join();
    check(ok && engine_allocations == 0, "S6 direct loop: 2,000 orders and ExecutionReports in steady state, " +
                                             std::to_string(engine_allocations.load()) + " allocations on the engine thread");
}

void pipeline(const std::string& dir) {
    tcp_listener l;
    l.open(0);
    journal_file f;
    f.create(dir + "/pipeline.journal", 1, 8u << 20);
    yes y;
    auto in = std::make_unique<app_ring>(), out = std::make_unique<app_ring>();
    std::atomic<bool> stop{false};
    std::unique_ptr<pipeline_engine<yes>> p;
    engine_config ec;
    ec.port = l.port();
    std::thread t([&] {
        p = std::make_unique<pipeline_engine<yes>>(client(), ec, f, y, *in, *out);
        p->run(stop);  // the session stage here, the journal and sender stages on threads of its own
    });
    engine_allocations = 0;
    const bool ok = exchange(l, *in, *out, [] { return true; }, 200, 2000);
    stop = true;
    t.join();
    check(ok && engine_allocations == 0, "S6 pipeline, all three stages: 2,000 orders and ExecutionReports in steady state, " +
                                             std::to_string(engine_allocations.load()) + " allocations");
}

}  // namespace

int main() {
    test_thread = true;
    char t[] = "engine_alloc_test_XXXXXX";
    const char* d = ::mkdtemp(t);
    if (!d) {
        check(false, "temporary directory");
        return test::result();
    }
    direct(d);
    pipeline(d);
    std::filesystem::remove_all(d);
    return test::result();
}
