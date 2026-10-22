// A session made recoverable (note 004): every input journaled and made
// durable before any of its outputs is released (D1), only ticks that did
// something journaled (D2), sent messages kept in memory and rebuilt by
// replay (D3). Design: docs/design/004-store.md
//
// Strategy (durability, note 004 A to D; durability.hpp):
//   bool append(record_kind, session_time, std::string_view a, std::string_view b);
//   durability make_durable(session_time);  durable: everything appended is as safe as
//                                           the strategy promises; pending: not yet (B)
//   durability poll(session_time);          between inputs: B's time bound
// Out (downstream of the held outputs):
//   void write(std::string_view bytes);                     to the connection
//   void deliver(std::string_view message, bool replayed);  an application message, in order;
//                                                           replayed: during recovery
//   void disconnect();
// Policy (the application's decisions, note 003's hooks). They must be
// deterministic, decided from their arguments alone, or replay could decide
// differently from the original run:
//   void log(session_log, std::string_view, std::uint64_t);
//   bool resend(const message_view<>&);
//   bool release(std::string_view msg_type, std::string_view body);
#pragma once

#include <fixgw/durability.hpp>
#include <fixgw/journal.hpp>
#include <fixgw/session.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace fixgw {

// Sent messages, in order, by sequence number (note 003's Store). Memory is
// reserved at construction; past the reservation it grows (an allocation,
// amortised; size the reservation for a session's day).
class memory_store {
public:
    explicit memory_store(std::size_t reserve_bytes = 64u << 20, std::size_t reserve_messages = 1u << 20) {
        bytes_.reserve(reserve_bytes);
        index_.reserve(reserve_messages);
    }
    void record(std::uint64_t seq, std::string_view b) {
        while (index_.size() + 1 < seq) index_.push_back({bytes_.size(), 0});  // never expected: numbers are dense
        if (seq != index_.size() + 1) return;
        index_.push_back({bytes_.size(), static_cast<std::uint32_t>(b.size())});
        bytes_.insert(bytes_.end(), b.begin(), b.end());
    }
    std::string_view lookup(std::uint64_t seq) const {
        if (seq == 0 || seq > index_.size()) return {};
        const auto [at, n] = index_[seq - 1];
        return n ? std::string_view(bytes_.data() + at, n) : std::string_view{};
    }
    void reset() {
        bytes_.clear();
        index_.clear();
    }
    std::uint64_t last() const noexcept { return index_.size(); }

private:
    std::vector<char> bytes_;
    std::vector<std::pair<std::size_t, std::uint32_t>> index_;
};

// The session's sink: holds its outputs until the input behind them is durable.
template <class Policy>
class held_sink {
public:
    explicit held_sink(Policy& p) : policy_(p) {
        out_.reserve(1u << 20);
        delivered_.reserve(1u << 20);
        ends_.reserve(4096);
    }
    void write(std::string_view b) { out_.insert(out_.end(), b.begin(), b.end()); }
    void disconnect() { disconnect_ = true; }
    void deliver(const message_view<>& m) {  // copied: the view lasts only the call
        const std::string_view b = m.bytes();
        delivered_.insert(delivered_.end(), b.begin(), b.end());
        ends_.push_back(delivered_.size());
    }
    void log(session_log l, std::string_view what, std::uint64_t seq) { policy_.log(l, what, seq); }
    bool resend(const message_view<>& m) { return policy_.resend(m); }
    bool release(std::string_view t, std::string_view b) { return policy_.release(t, b); }

    bool empty() const noexcept { return out_.empty() && ends_.empty() && !disconnect_; }
    template <class Out>
    void release_to(Out& out, bool replayed) {
        if (!replayed && !out_.empty()) out.write(std::string_view(out_.data(), out_.size()));
        std::size_t from = 0;
        for (const std::size_t end : ends_) {
            out.deliver(std::string_view(delivered_.data() + from, end - from), replayed);
            from = end;
        }
        if (!replayed && disconnect_) out.disconnect();
        clear();
    }
    void clear() noexcept {
        out_.clear();
        delivered_.clear();
        ends_.clear();
        disconnect_ = false;
    }

private:
    Policy& policy_;
    std::vector<char> out_, delivered_;
    std::vector<std::size_t> ends_;
    bool disconnect_ = false;
};

template <class Strategy, class Out, class Policy>
class journaled_session {
public:
    journaled_session(const session_config& cfg, Strategy& journal, Out& out, Policy& policy)
        : journal_(journal), out_(out), held_(policy), session_(cfg, held_, store_) {}

    const session<held_sink<Policy>, memory_store>& state() const noexcept { return session_; }
    const memory_store& store() const noexcept { return store_; }
    bool failed() const noexcept { return failed_; }
    bool holding() const noexcept { return pending_; }

    // Between inputs: releases a group whose time bound has passed (B).
    void poll(session_time t) {
        if (!pending_ || failed_) return;
        settle(journal_.poll(t));
    }

    void on_connected(session_time t) {
        session_.on_connected(t);
        commit(record_kind::connected, t);
    }
    void on_disconnected(session_time t) {
        session_.on_disconnected(t);
        commit(record_kind::disconnected, t);
    }
    std::size_t on_bytes(const char* p, std::size_t n, session_time t) {
        const std::size_t used = session_.on_bytes(p, n, t);
        if (used > 0) commit(record_kind::bytes, t, std::string_view(p, used));  // exactly what was consumed
        return used;
    }
    void on_tick(session_time t) {  // D2: journaled only if it did something
        const auto before = snapshot();
        session_.on_tick(t);
        if (!held_.empty() || snapshot() != before) commit(record_kind::tick, t);
    }
    send_result send(std::string_view msg_type, std::string_view body, session_time t) {
        const send_result r = session_.send(msg_type, body, t);
        if (r != send_result::refused) {
            char prefix[1 + 3];
            prefix[0] = static_cast<char>(msg_type.size());
            std::memcpy(prefix + 1, msg_type.data(), msg_type.size());
            commit(record_kind::send, t, std::string_view(prefix, 1 + msg_type.size()), body);
        }
        return r;
    }
    void logout(std::string_view why, session_time t) {
        session_.logout(why, t);
        commit(record_kind::logout, t, why);
    }

    // Recovery: every record of a cut journal fed back through the same
    // inputs, nothing journaled again and nothing written; application
    // messages passed on marked replayed. False if a record does not replay
    // as it was recorded (a bytes record not wholly consumed): a journal that
    // does not belong to this session or code.
    bool recover(const journal_reader& r) {
        replaying_ = true;
        bool ok = true;
        for (const journal_record& rec : r.records()) {
            switch (rec.kind) {
                case record_kind::connected: on_connected(rec.time); break;
                case record_kind::disconnected: on_disconnected(rec.time); break;
                case record_kind::bytes:
                    ok = ok && on_bytes(rec.payload.data(), rec.payload.size(), rec.time) == rec.payload.size();
                    break;
                case record_kind::tick: on_tick(rec.time); break;
                case record_kind::send: {
                    const std::size_t tl = rec.payload.empty() ? 0 : static_cast<unsigned char>(rec.payload[0]);
                    ok = ok && tl >= 1 && tl <= 3 && rec.payload.size() >= 1 + tl;
                    if (ok) send(rec.payload.substr(1, tl), rec.payload.substr(1 + tl), rec.time);
                    break;
                }
                case record_kind::logout: logout(rec.payload, rec.time); break;
                default: ok = false;
            }
            if (!ok) break;
        }
        replaying_ = false;
        return ok;
    }

private:
    struct snap {
        session_state state;
        std::uint64_t out, in;
        std::size_t queued;
        bool operator!=(const snap& o) const noexcept {
            return state != o.state || out != o.out || in != o.in || queued != o.queued;
        }
    };
    snap snapshot() const noexcept {
        return {session_.state(), session_.next_outbound(), session_.next_expected(), session_.queued()};
    }

    // D1: the input made durable, then its outputs released (with B, those of
    // every input of the group, together). If the journal fails, nothing is
    // released and the connection is closed: carrying on would send what
    // recovery could not reproduce.
    void commit(record_kind k, session_time t, std::string_view a = {}, std::string_view b = {}) {
        if (replaying_) {
            held_.release_to(out_, true);
            return;
        }
        if (failed_) {
            held_.clear();
            return;
        }
        settle(journal_.append(k, t, a, b) ? journal_.make_durable(t) : durability::failed);
    }
    void settle(durability d) {
        if (d == durability::pending) {
            pending_ = true;
            return;
        }
        pending_ = false;
        if (d == durability::durable) {
            held_.release_to(out_, false);
            return;
        }
        held_.log(session_log::error, "journal write failed: nothing released, disconnecting", 0);
        failed_ = true;
        held_.clear();
        out_.disconnect();
    }

    Strategy& journal_;
    Out& out_;
    held_sink<Policy> held_;
    memory_store store_;
    session<held_sink<Policy>, memory_store> session_;
    bool replaying_ = false, failed_ = false, pending_ = false;
};

}  // namespace fixgw
