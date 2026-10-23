// The acceptor's front door (note 005): each new connection is held until its
// first whole message has arrived, which must be a Logon naming a configured
// session that is not already connected; otherwise the connection is closed
// with nothing sent (scenario 1S b, D6; 1S c). Design: docs/design/005-engine.md
#pragma once

#include <fixgw/decode.hpp>
#include <fixgw/framing.hpp>
#include <fixgw/transport.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace fixgw {

class acceptor_gate {
public:
    enum class refusal { not_logon, garbled, too_long, unknown_identity, already_connected, silent };

    // `max_first`: the longest first message accepted; `silence_ns`: how long
    // a connection may wait without completing one.
    explicit acceptor_gate(std::size_t max_first = 4096, std::int64_t silence_ns = 10'000'000'000)
        : max_first_(max_first), silence_ns_(silence_ns) {}

    // A session the gate admits: the counterparty's SenderCompID and
    // TargetCompID as its Logon states them (49, 56), and the session's id.
    void add(std::string_view their_sender, std::string_view their_target, int session_id) {
        known_.push_back({std::string(their_sender), std::string(their_target), session_id, false});
    }

    // Takes a new connection to hold until its first message.
    void hold(int fd, std::int64_t now_ns) { held_.push_back({fd, {}, now_ns}); }

    // Reads every held connection; for each with a whole first message,
    // either `admit(session_id, fd, bytes_read_so_far)` or closes it and
    // calls `refused(why)`. `admit` takes the descriptor.
    template <class Admit, class Refused>
    void poll(std::int64_t now_ns, Admit&& admit, Refused&& refused) {
        for (std::size_t i = 0; i < held_.size();) {
            held_conn& h = held_[i];
            char buf[1024];
            bool closed = false;
            for (;;) {
                const ssize_t n = ::recv(h.fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n > 0) {
                    h.bytes.append(buf, static_cast<std::size_t>(n));
                    if (h.bytes.size() > max_first_) break;
                    continue;
                }
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) closed = true;
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            const auto decided = decide(h, now_ns, closed);
            if (decided.done) {
                if (decided.session >= 0) {
                    admit(decided.session, h.fd, std::move(h.bytes));
                } else {
                    ::close(h.fd);
                    refused(decided.why);
                }
                held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }

    // The session has disconnected: its identity may connect again.
    void released(int session_id) {
        for (auto& k : known_)
            if (k.session == session_id) k.connected = false;
    }
    std::size_t held() const noexcept { return held_.size(); }

private:
    struct known_identity {
        std::string sender, target;
        int session;
        bool connected;
    };
    struct held_conn {
        int fd;
        std::string bytes;
        std::int64_t since;
    };
    struct decision {
        bool done;
        int session;  // admitted to, or -1
        refusal why;
    };

    decision decide(const held_conn& h, std::int64_t now_ns, bool closed) {
        const frame_result f = frame(h.bytes.data(), h.bytes.size(), max_first_);
        if (f.status == frame_status::garbled) return {true, -1, refusal::garbled};
        if (f.status == frame_status::need_more) {
            if (h.bytes.size() > max_first_) return {true, -1, refusal::too_long};
            if (closed || now_ns - h.since >= silence_ns_) return {true, -1, refusal::silent};
            return {false, -1, refusal::silent};
        }
        message_view<> v;
        if (!decoder<256>::decode(h.bytes.data(), f.length, v).ok || v.msg_type() != "A")
            return {true, -1, refusal::not_logon};
        const auto sender = v.get(49), target = v.get(56);
        for (auto& k : known_) {
            if (!sender || !target || *sender != k.sender || *target != k.target) continue;
            if (k.connected) return {true, -1, refusal::already_connected};  // D6: the first is untouched
            k.connected = true;
            return {true, k.session, refusal::silent};
        }
        return {true, -1, refusal::unknown_identity};
    }

    std::size_t max_first_;
    std::int64_t silence_ns_;
    std::vector<known_identity> known_;
    std::vector<held_conn> held_;
};

}  // namespace fixgw
