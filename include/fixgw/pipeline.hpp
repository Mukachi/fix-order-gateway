// The pipeline (note 005, D4), in the manner of the LMAX Disruptor: three
// stages on their own threads. The session stage runs the session and writes
// each input to the journal without waiting; the journal stage makes what has
// been written durable and publishes how far; the sender stage releases each
// output once that position has passed the input behind it. Note 004's rule
// holds by construction: the sender releases nothing the journal stage has not
// passed. One `fdatasync` covers every input written while it ran.
//
// The default loop (D4). The session stage owns the connection's lifecycle
// (connects with backoff, reads, notices loss); the sender owns writing; a
// connection changes hands through the stage ring, in order with the outputs:
// "attach" before the Logon's bytes, "close" after the old connection's last
// output, so only the sender closes a socket. The initiator role; the acceptor
// role is the direct loop's. Design: docs/design/005-engine.md
#pragma once

#include <fixgw/engine.hpp>
#include <fixgw/journal.hpp>
#include <fixgw/recovery.hpp>
#include <fixgw/transport.hpp>

#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/spsc.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>

#include <unistd.h>

namespace fixgw {

// The session stage's strategy: write, publish how far, never wait.
class pipeline_journal {
public:
    explicit pipeline_journal(journal_file& f) : f_(f), written_(f.end()), durable_(f.end()) {}
    bool append(record_kind k, session_time t, std::string_view a, std::string_view b) {
        if (!f_.append(k, t, a, b)) return false;
        written_.store(f_.end(), std::memory_order_release);
        return true;
    }
    // Durable is the journal stage's to say; the outputs carry the position
    // instead (tagged_out), so the session moves on.
    durability make_durable(session_time) { return durability::durable; }
    durability poll(session_time) { return durability::durable; }
    std::size_t end() const noexcept { return f_.end(); }

    // The journal stage: one pass; true if it made anything durable, false
    // on nothing to do. `failed` set if fdatasync fails.
    bool sync_once() {
        const std::size_t w = written_.load(std::memory_order_acquire);
        if (w <= durable_.load(std::memory_order_relaxed)) return false;
        if (::fdatasync(f_.fd()) != 0) {
            failed_.store(true, std::memory_order_release);
            return false;
        }
        if (report_) report_(report_ctx_, w);  // before anything waiting on it can move
        durable_.store(w, std::memory_order_release);
        syncs_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    std::size_t durable() const noexcept { return durable_.load(std::memory_order_acquire); }
    std::uint64_t syncs() const noexcept { return syncs_.load(std::memory_order_relaxed); }
    // Called with each new durable position, after the fdatasync and before
    // it is published (the crash tests simulate power loss from it).
    void on_durable(void (*f)(void*, std::size_t), void* ctx) noexcept {
        report_ = f;
        report_ctx_ = ctx;
    }
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }

private:
    journal_file& f_;
    alignas(128) std::atomic<std::size_t> written_;
    alignas(128) std::atomic<std::size_t> durable_;
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> syncs_{0};
    void (*report_)(void*, std::size_t) = nullptr;
    void* report_ctx_ = nullptr;
};

// One output on its way to the sender: tagged with the journal position its
// input reached. Bytes longer than a chunk travel in several.
struct stage_chunk {
    // attach and close carry a socket in `data` and are not outputs (position 0).
    enum : std::uint8_t { bytes = 1, message = 2, replayed = 3, disconnect = 4, attach = 5, close = 6 };
    std::uint64_t position;
    std::uint8_t kind;
    bool last;  // a message's final chunk
    std::uint16_t length;
    char data[500];
};
static_assert(sizeof(stage_chunk) == 512, "a stage chunk is 512 bytes");
using stage_ring = lfipc::spsc_ring<stage_chunk, 8192>;

template <class Policy>
class pipeline_engine {
public:
    struct cpus {
        int session = -1, journal = -1, sender = -1;  // -1: not pinned
    };

    // `file`: the journal, open at its end; `cfg`: where to connect (its wait
    // mode is not used: every stage busy-polls).
    pipeline_engine(const session_config& s, const engine_config& cfg, journal_file& file, Policy& policy, app_ring& in,
                    app_ring& out, cpus c = {})
        : cfg_(cfg), journal_(file), tagged_{*this}, us_(s, journal_, tagged_, policy), in_(in), out_(out), cpus_(c),
          ring_(std::make_unique<stage_ring>()) {
        inbox_.reserve(64u << 10);
    }

    bool recover(const journal_reader& r) { return recovered_ = us_.recover(r); }
    const auto& session() const noexcept { return us_; }
    std::uint64_t syncs() const noexcept { return journal_.syncs(); }
    void on_durable(void (*f)(void*, std::size_t), void* ctx) noexcept { journal_.on_durable(f, ctx); }
    // Whether the session is logged on: safe from any thread.
    bool connected() const noexcept { return connected_.load(std::memory_order_acquire); }

    // Runs the three stages until `stop`; this thread is the session stage.
    void run(const std::atomic<bool>& stop) {
        std::atomic<bool> done{false};
        std::thread journal([&] {
            if (cpus_.journal >= 0) lfipc::bench::detail::pin_to(cpus_.journal);
            while (!done.load(std::memory_order_relaxed)) journal_.sync_once();
            journal_.sync_once();
        });
        std::thread sender([&] {
            if (cpus_.sender >= 0) lfipc::bench::detail::pin_to(cpus_.sender);
            send_loop(done);
        });
        if (cpus_.session >= 0) lfipc::bench::detail::pin_to(cpus_.session);
        if (recovered_) us_.on_disconnected(clock_now());  // the old connection died with the old process
        while (!stop.load(std::memory_order_relaxed)) {
            const session_time now = clock_now();
            if (rfd_ < 0) connect(now);
            if (rfd_ >= 0) {
                char buf[16 << 10];
                ssize_t n;
                while ((n = ::recv(rfd_, buf, sizeof(buf), MSG_DONTWAIT)) > 0) inbox_.append(buf, static_cast<std::size_t>(n));
                if (!inbox_.empty()) inbox_.erase(0, us_.on_bytes(inbox_.data(), inbox_.size(), now));
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) lost(now);
            }
            app_slot slot;
            while (in_.try_pop(slot))
                us_.send(std::string_view(slot.type, slot.type_length), std::string_view(slot.data, slot.length), now);
            us_.on_tick(now);
            const bool up = rfd_ >= 0 && us_.state().state() == session_state::active;
            if (up) backoff_.reset();
            connected_.store(up, std::memory_order_release);
        }
        if (rfd_ >= 0) control(stage_chunk::close, rfd_);
        if (connecting_ >= 0) ::close(connecting_);
        done.store(true, std::memory_order_release);
        journal.join();
        sender.join();
    }

private:
    // The session's downstream: every output onto the stage ring, tagged.
    struct tagged_out {
        pipeline_engine& e;
        void write(std::string_view b) { e.put(stage_chunk::bytes, b); }
        void deliver(std::string_view m, bool replayed) {
            e.put(replayed ? stage_chunk::replayed : stage_chunk::message, m.substr(0, sizeof(app_slot::data)));
        }
        void disconnect() { e.put(stage_chunk::disconnect, {}); }
    };
    void put(std::uint8_t kind, std::string_view b) {
        const std::uint64_t pos = journal_.end();
        do {
            stage_chunk c;
            c.position = pos;
            c.kind = kind;
            c.length = static_cast<std::uint16_t>(b.size() < sizeof(c.data) ? b.size() : sizeof(c.data));
            std::memcpy(c.data, b.data(), c.length);
            b.remove_prefix(c.length);
            c.last = b.empty();
            while (!ring_->try_push(c)) {  // the sender is behind: wait for room
            }
        } while (!b.empty());
    }
    // A connection handed to the sender, or taken back: in order with the outputs.
    void control(std::uint8_t kind, int fd) {
        stage_chunk c;
        c.position = 0;
        c.kind = kind;
        c.last = true;
        c.length = sizeof(fd);
        std::memcpy(c.data, &fd, sizeof(fd));
        while (!ring_->try_push(c)) {
        }
    }

    // The session stage: connect with backoff (initiator).
    void connect(session_time now) {
        if (connecting_ < 0) {
            if (!backoff_.due(now.mono_ns)) return;
            connecting_ = connect_start(cfg_.port, cfg_.socket, cfg_.ip);
            if (connecting_ < 0) backoff_.failed(now.mono_ns);
            return;
        }
        switch (connect_check(connecting_)) {
            case connect_status::in_progress:
                return;
            case connect_status::failed:
                ::close(connecting_);
                connecting_ = -1;
                backoff_.failed(now.mono_ns);
                return;
            case connect_status::connected:
                rfd_ = connecting_;
                connecting_ = -1;
                inbox_.clear();
                control(stage_chunk::attach, rfd_);  // before the Logon's bytes
                us_.on_connected(now);
                return;
        }
    }
    void lost(session_time now) {
        control(stage_chunk::close, rfd_);  // after the old connection's last output
        rfd_ = -1;
        inbox_.clear();
        us_.on_disconnected(now);
        backoff_.failed(now.mono_ns);
    }

    // The sender stage: each chunk once the journal has passed its position.
    void send_loop(const std::atomic<bool>& done) {
        app_slot msg;
        std::size_t msg_len = 0;
        stage_chunk c;
        for (;;) {
            if (!ring_->try_pop(c)) {
                if (conn_.pending() > 0 && !conn_.flush()) shut();
                // One reading of `done` decides: reading it twice let a chunk
                // already sent be sent again when it turned true in between.
                if (!done.load(std::memory_order_acquire)) continue;
                if (!ring_->try_pop(c)) {  // done, and nothing left
                    conn_.flush();
                    conn_.close();
                    return;
                }
            }
            while (journal_.durable() < c.position) {  // note 004's rule
                if (journal_.failed()) return;
            }
            int fd;
            switch (c.kind) {
                case stage_chunk::bytes:
                    if (conn_.open() && !conn_.write(std::string_view(c.data, c.length))) shut();  // not reading
                    break;
                case stage_chunk::message:
                case stage_chunk::replayed:
                    std::memcpy(msg.data + msg_len, c.data, c.length);
                    msg_len += c.length;
                    if (c.last) {
                        msg.kind = c.kind == stage_chunk::message ? app_slot::message : app_slot::replayed;
                        msg.type_length = 0;
                        msg.length = static_cast<std::uint16_t>(msg_len);
                        while (!out_.try_push(msg)) {
                        }
                        msg_len = 0;
                    }
                    break;
                case stage_chunk::disconnect:
                    shut();
                    break;
                case stage_chunk::attach:
                    std::memcpy(&fd, c.data, sizeof(fd));
                    conn_.attach(fd);
                    break;
                case stage_chunk::close:
                    conn_.flush();
                    conn_.close();
                    break;
            }
        }
    }
    // Ends the connection from the writing side; the session stage sees
    // end-of-file and hands it back with "close".
    void shut() {
        if (conn_.open()) ::shutdown(conn_.fd(), SHUT_RDWR);
    }

    engine_config cfg_;
    pipeline_journal journal_;
    tagged_out tagged_;
    journaled_session<pipeline_journal, tagged_out, Policy> us_;
    app_ring& in_;
    app_ring& out_;
    cpus cpus_;
    std::unique_ptr<stage_ring> ring_;
    tcp_connection conn_;  // the sender's: writes only
    int rfd_ = -1, connecting_ = -1;  // the session stage's: the socket it reads, and one being connected
    backoff backoff_;
    std::string inbox_;
    std::atomic<bool> connected_{false};
    bool recovered_ = false;
};

}  // namespace fixgw
