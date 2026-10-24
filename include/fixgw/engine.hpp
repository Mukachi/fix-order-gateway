// The direct engine loop (note 005): one session on one pinned thread, its
// TCP connection, and the application's two rings (D5). Each pass reads the
// socket, takes the application's orders, ticks, and flushes what the kernel
// would not take; between passes it waits in one of three ways (D1). Note
// 004's rule holds as in journaled_session: nothing leaves before its input is
// durable. Design: docs/design/005-engine.md
#pragma once

#include <fixgw/recovery.hpp>
#include <fixgw/session.hpp>
#include <fixgw/transport.hpp>

#include <lfipc/spsc.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <string_view>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace fixgw {

// One slot of the application's rings: 512 bytes (D5). Note 007 adds the
// order's state behind each ExecutionReport and OrderCancelReject, a request
// refused before it left, and the application's logout.
struct app_slot {
    enum : std::uint8_t {
        order = 1,           // to the engine: an application message (D, F, G, ...)
        message = 2,         // from: a message as received
        replayed = 3,        // from: a message replayed in recovery
        order_state = 4,     // from: an order_event (its state, or not sent), in `data`
        replayed_state = 5,  // from: the same, in recovery
        refused = 6,         // from: an order_event: a request refused before it left
        logout = 7,          // to the engine: log out once logged on and caught up, then stop
    };
    std::uint8_t kind;
    std::uint8_t type_length;  // order: its MsgType's length
    std::uint16_t length;      // bytes of `data` in use
    char type[4];              // order: its MsgType
    char data[504];            // order: its body; message: the whole message as received
};
static_assert(sizeof(app_slot) == 512, "D5: 512-byte slots");

inline constexpr std::size_t app_ring_slots = 1024;
using app_ring = lfipc::spsc_ring<app_slot, app_ring_slots>;

// The application's side: an order into the session's ring, with the
// doorbell for an engine that blocks. False if it does not fit a slot or the
// ring is full.
inline bool submit_order(app_ring& ring, std::string_view msg_type, std::string_view body, int doorbell = -1) {
    if (msg_type.empty() || msg_type.size() > sizeof(app_slot::type) || body.size() > sizeof(app_slot::data)) return false;
    app_slot s;
    s.kind = app_slot::order;
    s.type_length = static_cast<std::uint8_t>(msg_type.size());
    s.length = static_cast<std::uint16_t>(body.size());
    std::memcpy(s.type, msg_type.data(), msg_type.size());
    std::memcpy(s.data, body.data(), body.size());
    if (!ring.try_push(s)) return false;
    if (doorbell >= 0) {
        const std::uint64_t one = 1;
        if (::write(doorbell, &one, sizeof(one)) < 0) {
        }
    }
    return true;
}

// The application's logout: once the session is logged on and caught up
// (reconnecting first if it must, so nothing is left in doubt), it logs out;
// the engine does not reconnect after, and its run ends once disconnected
// (note 007).
inline bool submit_logout(app_ring& ring, int doorbell = -1) {
    app_slot s;
    s.kind = app_slot::logout;
    s.type_length = 0;
    s.length = 0;
    if (!ring.try_push(s)) return false;
    if (doorbell >= 0) {
        const std::uint64_t one = 1;
        if (::write(doorbell, &one, sizeof(one)) < 0) {
        }
    }
    return true;
}

// An order event as a slot, and back.
inline app_slot event_slot(const order_event& e, std::uint8_t kind) noexcept {
    app_slot s;
    s.kind = kind;
    s.type_length = 0;
    s.length = sizeof(e);
    std::memcpy(s.data, &e, sizeof(e));
    return s;
}
inline order_event slot_event(const app_slot& s) noexcept {
    order_event e;
    std::memcpy(&e, s.data, sizeof(e));
    return e;
}
static_assert(sizeof(order_event) <= sizeof(app_slot::data), "an order event fits a slot");

// What the application learns of a request the gateway refused.
inline order_event refusal_event(std::string_view body, const order_book::verdict& v) noexcept {
    order_event e{};
    e.kind = order_event::refused;
    const std::string_view id = detail::body_field(body, 11).value_or("");
    e.id_length = static_cast<std::uint8_t>(id.size() < sizeof(e.cl_ord_id) ? id.size() : sizeof(e.cl_ord_id));
    std::memcpy(e.cl_ord_id, id.data(), e.id_length);
    const std::string_view why = v.ok ? std::string_view("the session is not taking messages") : std::string_view(v.why);
    e.reason_length = static_cast<std::uint8_t>(why.size() < sizeof(e.reason) ? why.size() : sizeof(e.reason));
    std::memcpy(e.reason, why.data(), e.reason_length);
    e.retry_at_ns = v.retry_at_ns;
    return e;
}

enum class wait_mode { busy, block, spin_then_block };

inline session_time clock_now() noexcept {
    timespec m, r;
    ::clock_gettime(CLOCK_MONOTONIC, &m);
    ::clock_gettime(CLOCK_REALTIME, &r);
    return {m.tv_sec * 1'000'000'000LL + m.tv_nsec, r.tv_sec * 1000LL + r.tv_nsec / 1'000'000};
}

struct engine_config {
    wait_mode wait = wait_mode::busy;
    std::int64_t spin_ns = 50'000;             // spin_then_block: how long to spin after the last activity
    std::int64_t block_timeout_ns = 1'000'000;  // block: the longest wait, so ticks and B's bound still fire
    socket_options socket{};
    std::uint16_t port = 0;                // initiator: where to connect
    std::uint32_t ip = INADDR_LOOPBACK;    // host order
};

template <class Strategy, class Policy>
class engine {
public:
    engine(const session_config& session, const engine_config& cfg, Strategy& journal, Policy& policy, app_ring& in,
           app_ring& out, const order_config& orders = {})
        : cfg_(cfg), out_adapter_{*this}, us_(session, journal, out_adapter_, policy, orders), in_(in), out_(out),
          initiator_(session.role == session_role::initiator) {
        doorbell_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        epoll_ = ::epoll_create1(EPOLL_CLOEXEC);
        epoll_event e{};
        e.events = EPOLLIN;
        e.data.fd = doorbell_;
        ::epoll_ctl(epoll_, EPOLL_CTL_ADD, doorbell_, &e);
        inbox_.reserve(64u << 10);
    }
    engine(const engine&) = delete;
    engine& operator=(const engine&) = delete;
    ~engine() {
        ::close(doorbell_);
        ::close(epoll_);
    }

    int doorbell() const noexcept { return doorbell_; }
    const auto& session() const noexcept { return us_; }
    // Whether a connection is up: safe from any thread (the connection itself
    // is the engine thread's alone).
    bool connected() const noexcept { return connected_.load(std::memory_order_acquire); }
    // The application asked to log out, and the connection is gone: safe from
    // any thread (the engine thread publishes it).
    bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }

    // Recovery before the loop starts (note 004): every message replayed goes
    // to the application's ring marked replayed.
    bool recover(const journal_reader& r) { return us_.recover(r); }

    // Acceptor: a connection the gate admitted, with the bytes it read.
    void adopt(int fd, std::string_view first_bytes, session_time now) {
        attach(fd);
        us_.on_connected(now);
        inbox_.assign(first_bytes.begin(), first_bytes.end());
        consume(now);
    }

    // One pass: connect if due, read, take orders, tick, flush. True if it
    // moved anything (the spin_then_block mode counts from the last such pass).
    bool step(session_time now) {
        bool moved = false;
        // After a logout is sent, no reconnecting; before, the session must
        // reconnect and catch up first, so nothing is left in doubt.
        if (initiator_ && !conn_.open() && !(logging_out_ && !logout_pending_)) moved |= try_connect(now);
        if (connecting_ >= 0) moved |= finish_connect(now);
        if (conn_.open() && connecting_ < 0) {
            char buf[16 << 10];
            long n;
            while ((n = conn_.read_some(buf, sizeof(buf))) > 0) {
                inbox_.append(buf, static_cast<std::size_t>(n));
                moved = true;
            }
            if (!inbox_.empty()) consume(now);
            if (n < 0) lost(now);
        }
        app_slot s;
        while (in_.try_pop(s)) {
            moved = true;
            if (s.kind == app_slot::logout) {  // sent once logged on and caught up (below)
                logging_out_ = logout_pending_ = true;
                continue;
            }
            const std::string_view body(s.data, s.length);
            if (us_.send(std::string_view(s.type, s.type_length), body, now) == send_result::refused)
                push_out(event_slot(refusal_event(body, us_.last_refusal()), app_slot::refused));
        }
        us_.on_tick(now);
        if (logout_pending_ && us_.state().state() == session_state::active) {
            us_.logout_when_synced(now);
            logout_pending_ = false;
        }
        us_.poll(now);
        if (conn_.open() && conn_.pending() > 0 && !conn_.flush()) lost(now);
        moved |= drain_overflow();
        if (us_.state().state() == session_state::active) backoff_.reset();
        connected_.store(conn_.open() && connecting_ < 0, std::memory_order_release);
        finished_.store(logging_out_ && !logout_pending_ && !conn_.open() && connecting_ < 0, std::memory_order_release);
        return moved;
    }

    // Runs until `stop`; waits between passes as configured (D1).
    void run(const std::atomic<bool>& stop) {
        std::int64_t last_activity = clock_now().mono_ns;
        while (!stop.load(std::memory_order_relaxed) && !finished()) {
            const session_time now = clock_now();
            if (step(now)) last_activity = now.mono_ns;
            if (cfg_.wait == wait_mode::busy) continue;
            if (cfg_.wait == wait_mode::spin_then_block && now.mono_ns - last_activity < cfg_.spin_ns) continue;
            wait(cfg_.block_timeout_ns);
        }
    }

private:
    struct out_to_wire {
        engine& e;
        void write(std::string_view b) {
            if (!e.conn_.write(b)) e.not_reading_ = true;
        }
        void deliver(std::string_view m, bool replayed) { e.to_application(m, replayed); }
        void order_update(const order_event& ev, bool replayed) {
            e.push_out(event_slot(ev, replayed ? app_slot::replayed_state : app_slot::order_state));
        }
        void disconnect() { e.conn_.close(); }
    };

    void attach(int fd) {
        conn_.attach(fd);
        epoll_event e{};
        e.events = EPOLLIN;
        e.data.fd = fd;
        ::epoll_ctl(epoll_, EPOLL_CTL_ADD, fd, &e);
    }
    bool try_connect(session_time now) {
        if (connecting_ >= 0 || !backoff_.due(now.mono_ns)) return false;
        connecting_ = connect_start(cfg_.port, cfg_.socket, cfg_.ip);
        if (connecting_ < 0) backoff_.failed(now.mono_ns);
        return true;
    }
    bool finish_connect(session_time now) {
        switch (connect_check(connecting_)) {
            case connect_status::in_progress:
                return false;
            case connect_status::failed:
                ::close(connecting_);
                connecting_ = -1;
                backoff_.failed(now.mono_ns);
                return true;
            case connect_status::connected:
                attach(connecting_);
                connecting_ = -1;
                inbox_.clear();
                us_.on_connected(now);
                return true;
        }
        return false;
    }
    void consume(session_time now) {
        const std::size_t used = us_.on_bytes(inbox_.data(), inbox_.size(), now);
        inbox_.erase(0, used);
        if (not_reading_) {  // the counterparty is not reading what we send
            us_.logout("not reading", now);
            lost(now);
        }
    }
    void lost(session_time now) {
        // Our Logout unanswered when the connection went: not logged out yet;
        // reconnect, catch up and log out again (nothing left in doubt).
        if (logging_out_ && us_.state().logging_out()) logout_pending_ = true;
        if (conn_.open()) ::epoll_ctl(epoll_, EPOLL_CTL_DEL, conn_.fd(), nullptr);
        conn_.close();
        inbox_.clear();
        not_reading_ = false;
        us_.on_disconnected(now);
        if (initiator_) backoff_.failed(now.mono_ns);
    }
    void to_application(std::string_view m, bool replayed) {
        app_slot s;
        s.kind = replayed ? app_slot::replayed : app_slot::message;
        s.type_length = 0;
        const std::size_t n = m.size() < sizeof(s.data) ? m.size() : sizeof(s.data);  // longer: cut (D5)
        s.length = static_cast<std::uint16_t>(n);
        std::memcpy(s.data, m.data(), n);
        push_out(s);
    }
    void push_out(const app_slot& s) {
        if (overflow_.empty() && out_.try_push(s)) return;
        overflow_.push_back(s);  // the application is behind: kept in order, never dropped
    }
    bool drain_overflow() {
        bool moved = false;
        while (!overflow_.empty() && out_.try_push(overflow_.front())) {
            overflow_.pop_front();
            moved = true;
        }
        return moved;
    }
    void wait(std::int64_t timeout_ns) {
        epoll_event ev[4];
        const int ms = static_cast<int>(timeout_ns / 1'000'000);
        const int n = ::epoll_wait(epoll_, ev, 4, ms > 0 ? ms : 1);
        for (int i = 0; i < n; ++i)
            if (ev[i].data.fd == doorbell_) {
                std::uint64_t v;
                if (::read(doorbell_, &v, sizeof(v)) < 0) {
                }
            }
    }

    engine_config cfg_;
    tcp_connection conn_;
    out_to_wire out_adapter_;
    journaled_session<Strategy, out_to_wire, Policy> us_;
    app_ring& in_;
    app_ring& out_;
    bool initiator_;
    int connecting_ = -1, doorbell_ = -1, epoll_ = -1;
    bool not_reading_ = false, logging_out_ = false, logout_pending_ = false;
    std::atomic<bool> connected_{false}, finished_{false};
    backoff backoff_;
    std::string inbox_;
    std::deque<app_slot> overflow_;
};

}  // namespace fixgw
