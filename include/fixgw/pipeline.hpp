// The pipeline (note 005, D4), in the manner of the LMAX Disruptor: three
// stages on their own threads. The session stage runs the session and writes
// each input to the journal without waiting; the journal stage makes what has
// been written durable and publishes how far; the sender stage releases each
// output once that position has passed the input behind it. Note 004's rule
// holds by construction: the sender releases nothing the journal stage has not
// passed. One `fdatasync` covers every input written while it ran.
//
// Built for measurement against the direct loop: one connection's lifetime (a
// lost connection ends the run; reconnection is the direct loop's). Design:
// docs/design/005-engine.md
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
        durable_.store(w, std::memory_order_release);
        syncs_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    std::size_t durable() const noexcept { return durable_.load(std::memory_order_acquire); }
    std::uint64_t syncs() const noexcept { return syncs_.load(std::memory_order_relaxed); }
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }

private:
    journal_file& f_;
    alignas(128) std::atomic<std::size_t> written_;
    alignas(128) std::atomic<std::size_t> durable_;
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> syncs_{0};
};

// One output on its way to the sender: tagged with the journal position its
// input reached. Bytes longer than a chunk travel in several.
struct stage_chunk {
    enum : std::uint8_t { bytes = 1, message = 2, replayed = 3, disconnect = 4 };
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

    // `fd`: an established connection; `file`: the journal, open at its end.
    pipeline_engine(const session_config& s, journal_file& file, Policy& policy, app_ring& in, app_ring& out, int fd,
                    cpus c = {})
        : journal_(file), tagged_{*this}, us_(s, journal_, tagged_, policy), in_(in), out_(out), cpus_(c),
          ring_(std::make_unique<stage_ring>()) {
        conn_.attach(fd);
        inbox_.reserve(64u << 10);
    }

    bool recover(const journal_reader& r) { return recovered_ = us_.recover(r); }
    const auto& session() const noexcept { return us_; }
    std::uint64_t syncs() const noexcept { return journal_.syncs(); }

    // Runs the three stages until `stop` or the connection is lost; this
    // thread is the session stage.
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
        us_.on_connected(clock_now());
        while (!stop.load(std::memory_order_relaxed) && !lost_) {
            const session_time now = clock_now();
            char buf[16 << 10];
            long n;
            while ((n = conn_.read_some(buf, sizeof(buf))) > 0) inbox_.append(buf, static_cast<std::size_t>(n));
            if (!inbox_.empty()) inbox_.erase(0, us_.on_bytes(inbox_.data(), inbox_.size(), now));
            if (n < 0) {
                us_.on_disconnected(now);
                lost_ = true;
            }
            app_slot slot;
            while (in_.try_pop(slot))
                us_.send(std::string_view(slot.type, slot.type_length), std::string_view(slot.data, slot.length), now);
            us_.on_tick(now);
        }
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

    // The sender stage: each chunk once the journal has passed its position.
    void send_loop(const std::atomic<bool>& done) {
        app_slot msg;
        std::size_t msg_len = 0;
        stage_chunk c;
        for (;;) {
            if (!ring_->try_pop(c)) {
                if (conn_.pending() > 0) conn_.flush();
                if (done.load(std::memory_order_acquire) && !ring_->try_pop(c)) return;
                if (!done.load(std::memory_order_relaxed)) continue;
            }
            while (journal_.durable() < c.position) {  // note 004's rule
                if (journal_.failed()) return;
            }
            switch (c.kind) {
                case stage_chunk::bytes:
                    conn_.write(std::string_view(c.data, c.length));
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
                    ::shutdown(conn_.fd(), SHUT_RDWR);
                    break;
            }
        }
    }

    pipeline_journal journal_;
    tagged_out tagged_;
    journaled_session<pipeline_journal, tagged_out, Policy> us_;
    app_ring& in_;
    app_ring& out_;
    cpus cpus_;
    std::unique_ptr<stage_ring> ring_;
    tcp_connection conn_;
    std::string inbox_;
    bool lost_ = false, recovered_ = false;
};

}  // namespace fixgw
