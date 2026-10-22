// Paired session fuzzer (note 003, S4): an initiator and an acceptor talking
// through in-memory pipes, driven by a script read from the fuzz input:
// deliver bytes either way in any split, send application messages, advance
// the clock, corrupt or inject bytes, forge well-framed messages with chosen
// header values (so adversarial sequence and session cases are reached, which
// raw bytes almost never frame into), drop and restore the connection, log out.
// After every step the invariants are checked; any breach aborts.
//   outbound numbers advance by exactly one per message recorded (from 1 after
//   a reset); the expected inbound number never falls except at a logon or a
//   sequence reset (D4); each application message is delivered at most once,
//   in sequence order.
#include <fixgw/encode.hpp>
#include <fixgw/session.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using namespace fixgw;

struct side;

struct pipe_sink {
    std::string* out = nullptr;  // bytes towards the other side
    bool dropped = false;
    bool logged_on = false;      // a logon since the last check
    std::uint64_t last_delivered = 0;
    void write(std::string_view b) { out->append(b); }
    void disconnect() { dropped = true; }
    void deliver(const message_view<>& m) {
        const auto s = detail::parse_uint(m.get(34).value_or(std::string_view{}));
        if (!s || *s <= last_delivered) {
            std::fprintf(stderr, "delivered out of order or twice: %llu after %llu\n",
                         static_cast<unsigned long long>(s.value_or(0)), static_cast<unsigned long long>(last_delivered));
            std::abort();
        }
        last_delivered = *s;
    }
    void log(session_log, std::string_view what, std::uint64_t) {
        if (what == "logged on") {
            logged_on = true;
            last_delivered = 0;
        }
    }
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};

// Keeps only the newest messages (a ring), so long runs stay small; lookups
// of older ones find nothing and are gap-filled, which is legitimate.
struct ring_store {
    static constexpr std::size_t slots = 64;
    std::string bytes[slots];
    std::uint64_t seq[slots] = {};
    std::uint64_t last = 0;
    bool was_reset = false;  // since the last check
    void record(std::uint64_t s, std::string_view b) {
        if (s != last + 1) {
            std::fprintf(stderr, "recorded %llu after %llu\n", static_cast<unsigned long long>(s),
                         static_cast<unsigned long long>(last));
            std::abort();
        }
        last = s;
        bytes[s % slots].assign(b);
        seq[s % slots] = s;
    }
    std::string_view lookup(std::uint64_t s) {
        return seq[s % slots] == s ? std::string_view(bytes[s % slots]) : std::string_view{};
    }
    void reset() {
        last = 0;
        was_reset = true;
        for (auto& x : seq) x = 0;
    }
};

session_config config(session_role r, bool reset) {
    session_config c;
    c.reset_on_logon = reset;
    c.role = r;
    c.sender_comp_id = r == session_role::initiator ? "CLIENT" : "VENUE";
    c.target_comp_id = r == session_role::initiator ? "VENUE" : "CLIENT";
    c.heartbeat_s = 5;
    c.queue_messages = 8;
    c.max_frame = 4096;
    return c;
}

struct side {
    pipe_sink sink;
    ring_store store;
    session<pipe_sink, ring_store> s;
    std::string inbox;  // bytes received, not yet consumed (a partial message)
    std::uint64_t expected = 1;
    side(session_role r, bool reset) : s(config(r, reset), sink, store) {}
    void check() {
        const std::uint64_t e = s.next_expected();
        if (e < expected && !sink.logged_on && !store.was_reset) {
            std::fprintf(stderr, "expected inbound fell from %llu to %llu\n", static_cast<unsigned long long>(expected),
                         static_cast<unsigned long long>(e));
            std::abort();
        }
        if (s.next_outbound() != store.last + 1) {
            std::fprintf(stderr, "next outbound %llu, last recorded %llu\n",
                         static_cast<unsigned long long>(s.next_outbound()), static_cast<unsigned long long>(store.last));
            std::abort();
        }
        expected = e;
        sink.logged_on = false;
        store.was_reset = false;
    }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string a_to_b, b_to_a;
    // The first byte: whether the initiator resets sequence numbers at logon (D4).
    const bool reset = size > 0 && (data[0] & 1);
    side a(session_role::initiator, reset), b(session_role::acceptor, false);
    a.sink.out = &a_to_b;
    b.sink.out = &b_to_a;
    std::int64_t ms = 0;
    const auto now = [&] { return session_time{ms * 1'000'000, 1'792'567'800'000 + ms}; };
    const auto connect = [&] {
        a_to_b.clear();
        b_to_a.clear();
        a.inbox.clear();
        b.inbox.clear();
        a.sink.dropped = b.sink.dropped = false;
        b.s.on_connected(now());
        a.s.on_connected(now());
    };
    const auto drop = [&] {
        a.s.on_disconnected(now());
        b.s.on_disconnected(now());
    };
    const auto deliver = [&](std::string& pipe, side& to, std::size_t n) {
        if (n > pipe.size()) n = pipe.size();
        to.inbox.append(pipe, 0, n);
        pipe.erase(0, n);
        const std::size_t used = to.s.on_bytes(to.inbox.data(), to.inbox.size(), now());
        to.inbox.erase(0, used);
    };
    connect();
    std::uint64_t order = 0;
    std::size_t i = 1;
    const auto next = [&]() -> std::uint8_t { return i < size ? data[i++] : 0; };
    // A well-framed message from `from_initiator`'s side, its parts chosen by
    // the input: MsgType, MsgSeqNum near the receiver's expected number, and
    // optional session fields.
    const auto forge = [&](bool from_initiator) {
        static constexpr const char* types[] = {"0", "1", "2", "3", "4", "5", "A", "D"};
        const std::string_view type = types[next() % 8];
        side& to = from_initiator ? b : a;
        const std::int64_t delta = static_cast<std::int8_t>(next()) % 4;
        const std::uint8_t flags = next(), more = next();
        const std::int64_t seq = static_cast<std::int64_t>(to.s.next_expected()) + delta;
        char buf[512], ts[utc_timestamp_length], orig[utc_timestamp_length];
        format_utc_timestamp(now().utc_ms + (flags & 0x80 ? 200'000 : 0), ts);
        format_utc_timestamp(now().utc_ms + (more & 1 ? 1000 : -1000), orig);
        encoder e(buf, sizeof(buf));
        e.begin(type);
        e.add(49, from_initiator ? "CLIENT" : (more & 2 ? "OTHER" : "VENUE"));
        e.add(56, from_initiator ? "VENUE" : "CLIENT");
        e.add(34, static_cast<std::uint64_t>(seq < 1 ? 1 : seq));
        e.add(52, std::string_view(ts, sizeof(ts)));
        if (flags & 1) e.add(43, 'Y');
        if (flags & 2) e.add(122, std::string_view(orig, sizeof(orig)));
        if (type == "A") {
            e.add(98, std::uint64_t{more & 4 ? 1u : 0u});
            if (!(more & 8)) e.add(108, std::uint64_t{5});
            if (flags & 4) e.add(141, 'Y');
        }
        if (type == "4") {
            if (flags & 8) e.add(123, 'Y');
            if (!(more & 16)) e.add(36, static_cast<std::uint64_t>(seq + static_cast<std::int8_t>(next()) % 6 + 2));
        }
        if (type == "1" && !(more & 32)) e.add(112, "T1");
        if (type == "2") {
            if (!(more & 64)) e.add(7, std::uint64_t{1u + (next() % 4)});
            e.add(16, static_cast<std::uint64_t>(next() % 3));
        }
        if (type == "D") e.add(11, "FORGED");
        const std::string_view m = e.finish();
        (from_initiator ? a_to_b : b_to_a).append(m);
    };
    while (i < size) {
        switch (next() % 12) {
            case 0: ms += next() * 100; break;
            case 1: deliver(a_to_b, b, next() + 1u); break;
            case 2: deliver(b_to_a, a, next() + 1u); break;
            case 3:
            case 4: {
                const std::string body = "11=" + std::to_string(++order) + "\x01" "55=VOD.L\x01";
                (data[i - 1] % 12 == 3 ? a : b).s.send("D", body, now());
                break;
            }
            case 5:
                if (!a_to_b.empty()) a_to_b[next() % a_to_b.size()] ^= static_cast<char>(next() | 1);
                break;
            case 6: {
                const std::size_t n = next() % 64;
                for (std::size_t k = 0; k < n && i < size; ++k) a_to_b.push_back(static_cast<char>(next()));
                break;
            }
            case 7:
                drop();
                connect();
                break;
            case 8: (next() & 1 ? a : b).s.logout("fuzz", now()); break;
            case 9:
                a.s.on_tick(now());
                b.s.on_tick(now());
                break;
            case 11: forge(next() & 1); break;
            case 10: {  // drain both pipes completely
                deliver(a_to_b, b, a_to_b.size());
                deliver(b_to_a, a, b_to_a.size());
                break;
            }
        }
        if (a.sink.dropped || b.sink.dropped) {  // either side closed the connection
            drop();
            a.sink.dropped = b.sink.dropped = false;
        }
        a.check();
        b.check();
    }
    return 0;
}
