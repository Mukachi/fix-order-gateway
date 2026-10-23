// Test helpers for the engines (note 005): a journal that keeps nothing, a
// policy that says yes, and a counterparty that answers each order with an
// ExecutionReport naming it.
#pragma once

#include <fixgw/recovery.hpp>
#include <fixgw/session.hpp>
#include <fixgw/transport.hpp>

#include <map>
#include <string>
#include <vector>

namespace fixgw::test {

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

}  // namespace fixgw::test
