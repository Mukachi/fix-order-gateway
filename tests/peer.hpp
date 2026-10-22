// Test helpers for the journaled session (note 004): its downstream (`wire`),
// a policy that always says yes, and a plain counterparty session joined to it
// through in-memory pipes.
#pragma once

#include <fixgw/recovery.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace fixgw::test {

struct wire {
    std::string sent;
    std::vector<std::string> delivered;
    int replayed = 0, disconnects = 0;
    void write(std::string_view b) { sent.append(b); }
    void deliver(std::string_view m, bool r) {
        delivered.emplace_back(m);
        replayed += r;
    }
    void disconnect() { ++disconnects; }
};

struct policy {
    void log(session_log, std::string_view, std::uint64_t) {}
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};

// The counterparty: a plain session (note 003) with a sink that collects.
struct peer_sink {
    std::string sent;
    int delivered = 0;
    void write(std::string_view b) { sent.append(b); }
    void disconnect() {}
    void deliver(const message_view<>&) { ++delivered; }
    void log(session_log, std::string_view, std::uint64_t) {}
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};
struct peer_store {
    std::map<std::uint64_t, std::string> m;
    void record(std::uint64_t s, std::string_view b) { m[s] = std::string(b); }
    std::string_view lookup(std::uint64_t s) {
        const auto it = m.find(s);
        return it == m.end() ? std::string_view{} : std::string_view(it->second);
    }
    void reset() { m.clear(); }
};

constexpr std::int64_t base_ms = 1'792'567'800'000;
session_time at(std::int64_t s) { return {s * 1'000'000'000, base_ms + s * 1000}; }

session_config ours() {
    session_config c;
    c.role = session_role::initiator;
    c.sender_comp_id = "CLIENT";
    c.target_comp_id = "VENUE";
    return c;
}
session_config theirs() {
    session_config c;
    c.role = session_role::acceptor;
    c.sender_comp_id = "VENUE";
    c.target_comp_id = "CLIENT";
    return c;
}

struct peer_link {
    peer_sink ps;
    peer_store pst;
    session<peer_sink, peer_store> peer{theirs(), ps, pst};
    std::string inbox_ours, inbox_peer;
    // Moves everything written each way until both sides are quiet, in
    // chunks of `chunk` bytes (partial and coalesced reads).
    template <class Us>
    void pump(Us& us, wire& w, std::int64_t t, std::size_t chunk = 37) {
        for (int round = 0; round < 50 && (!w.sent.empty() || !ps.sent.empty()); ++round) {
            while (!w.sent.empty()) {
                inbox_peer.append(w.sent, 0, std::min(chunk, w.sent.size()));
                w.sent.erase(0, std::min(chunk, w.sent.size()));
                inbox_peer.erase(0, peer.on_bytes(inbox_peer.data(), inbox_peer.size(), at(t)));
            }
            while (!ps.sent.empty()) {
                inbox_ours.append(ps.sent, 0, std::min(chunk, ps.sent.size()));
                ps.sent.erase(0, std::min(chunk, ps.sent.size()));
                inbox_ours.erase(0, us.on_bytes(inbox_ours.data(), inbox_ours.size(), at(t)));
            }
        }
    }
};

}  // namespace fixgw::test
