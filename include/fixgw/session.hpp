// The FIX 4.4 session layer: one connection's state machine (logon and logout,
// sequence numbers, resend and gap fill, heartbeats and test requests),
// sans-I/O: bytes and clock readings in, bytes and actions out. Design:
// docs/design/003-session.md
//
// Sink, called as outputs arise (D3):
//   void write(std::string_view bytes);             bytes for the connection
//   void disconnect();                              close the connection
//   void deliver(const message_view<>& m);          an application message of a supported
//                                                   MsgType, in order, once
//   void log(session_log level, std::string_view what, std::uint64_t seq);
//   bool resend(const message_view<>& original);    an application message is about to be
//                                                   resent: true resends it, false gap-fills
//   bool release(std::string_view msg_type, std::string_view body);
//                                                   a message queued while not logged on is
//                                                   about to be sent: true sends, false drops
// Store, the messages this session sent (note 004 makes it durable):
//   void record(std::uint64_t seq, std::string_view bytes);
//   std::string_view lookup(std::uint64_t seq);     empty if it no longer has it
//   void reset();                                   sequence numbers restart at 1 (D4)
#pragma once

#include <fixgw/decode.hpp>
#include <fixgw/encode.hpp>
#include <fixgw/framing.hpp>
#include <fixgw/validate.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace fixgw {

// Both clocks the session reads: monotonic for its timers, UTC for SendingTime.
struct session_time {
    std::int64_t mono_ns;
    std::int64_t utc_ms;
};

enum class session_role { initiator, acceptor };
enum class session_state { disconnected, awaiting_logon, logon_sent, active, resending, logout_sent, logout_replied };
enum class send_result { sent, queued, refused };
enum class session_log { info, warning, error };

struct session_config {
    session_role role = session_role::initiator;
    std::string_view sender_comp_id, target_comp_id;  // ours and the counterparty's
    std::uint32_t heartbeat_s = 30;                   // HeartBtInt we propose (initiator)
    std::int64_t logon_timeout_ns = 10'000'000'000, logout_timeout_ns = 10'000'000'000;
    std::int64_t sending_time_tolerance_ms = 120'000;
    bool reset_on_logon = false;     // send ResetSeqNumFlag=Y at logon (D4)
    std::size_t queue_messages = 256;  // application messages held while not logged on (D2)
    std::size_t max_body = 1024;       // largest body accepted for the queue
    std::size_t max_frame = 64 * 1024;  // largest message, either way
    // Application MsgTypes this session takes (note 006, D4), space-separated:
    // another valid one is answered with BusinessMessageReject. By default the
    // order-entry messages (note 001) and BusinessMessageReject itself.
    std::string_view app_msg_types = "D F G 8 9 j";
};

namespace detail {

// A short text built without allocating, for Logout and Reject reasons.
class text {
public:
    text& operator<<(std::string_view s) noexcept {
        const std::size_t n = s.size() < sizeof(buf_) - len_ ? s.size() : sizeof(buf_) - len_;
        std::memcpy(buf_ + len_, s.data(), n);
        len_ += n;
        return *this;
    }
    text& operator<<(std::uint64_t v) noexcept {
        char tmp[20];
        char* p = put_uint(tmp + sizeof(tmp), v);
        return *this << std::string_view(p, static_cast<std::size_t>(tmp + sizeof(tmp) - p));
    }
    std::string_view view() const noexcept { return {buf_, len_}; }

private:
    char buf_[128];
    std::size_t len_ = 0;
};

inline bool is_admin(std::string_view t) noexcept {
    return t.size() == 1 && (t[0] == '0' || t[0] == '1' || t[0] == '2' || t[0] == '3' || t[0] == '4' ||
                             t[0] == '5' || t[0] == 'A');
}

}  // namespace detail

template <class Sink, class Store>
class session {
public:
    session(const session_config& cfg, Sink& sink, Store& store)
        : cfg_(cfg), sink_(sink), store_(store), out_(cfg.max_frame), queue_(cfg.queue_messages),
          queue_bytes_(cfg.queue_messages * cfg.max_body) {}

    session_state state() const noexcept { return state_; }
    std::uint64_t next_outbound() const noexcept { return next_out_; }
    std::uint64_t next_expected() const noexcept { return expected_; }
    std::size_t queued() const noexcept { return queue_count_; }

    void on_connected(session_time now) {
        connected_at_ = last_sent_ = last_recv_ = now.mono_ns;
        test_pending_ = false;
        gap_target_ = 0;
        if (cfg_.role == session_role::acceptor) {
            state_ = session_state::awaiting_logon;
            return;
        }
        if (cfg_.reset_on_logon) reset_sequences();
        heartbeat_ns_ = static_cast<std::int64_t>(cfg_.heartbeat_s) * 1'000'000'000;
        state_ = session_state::logon_sent;
        send_logon(now, cfg_.heartbeat_s, cfg_.reset_on_logon);
    }

    void on_disconnected(session_time) {
        sync_logout_ = false;
        state_ = session_state::disconnected;
        test_pending_ = false;
        gap_target_ = 0;
    }

    // Frames and handles every whole message at the start of the bytes;
    // returns how many bytes it used (the rest is a partial message, to be
    // offered again with more).
    std::size_t on_bytes(const char* p, std::size_t n, session_time now) {
        std::size_t used = 0;
        while (used < n && state_ != session_state::disconnected) {
            const frame_result f = frame(p + used, n - used, cfg_.max_frame);
            if (f.status == frame_status::need_more) break;
            if (f.status == frame_status::garbled) {
                if (wrong_version(p + used, n - used)) {  // scenario 2i
                    logout_and_disconnect("BeginString is not FIX.4.4", now);
                    return n;
                }
                sink_.log(session_log::warning, f.why, 0);
                used += f.length;
                continue;
            }
            last_recv_ = now.mono_ns;
            test_pending_ = false;
            on_message(p + used, f.length, now);
            used += f.length;
        }
        return used;
    }

    void on_tick(session_time now) {
        switch (state_) {
            case session_state::awaiting_logon:
            case session_state::logon_sent:
                if (now.mono_ns - connected_at_ >= cfg_.logon_timeout_ns) {
                    sink_.log(session_log::error, "no Logon in time", 0);
                    disconnect();
                }
                break;
            case session_state::active:
            case session_state::resending: {
                const std::int64_t limit = heartbeat_ns_ + heartbeat_ns_ / 5;  // HeartBtInt plus 20%
                if (test_pending_ && now.mono_ns - test_sent_at_ >= limit) {
                    sink_.log(session_log::error, "no reply to TestRequest", 0);
                    disconnect();
                    break;
                }
                if (!test_pending_ && now.mono_ns - last_recv_ >= limit) {
                    detail::text id;
                    id << ++test_id_;
                    test_pending_ = true;
                    test_sent_at_ = now.mono_ns;
                    emit("1", now, [&](encoder& e) { e.add(112, id.view()); });
                }
                if (now.mono_ns - last_sent_ >= heartbeat_ns_) emit("0", now, [](encoder&) {});
                break;
            }
            case session_state::logout_sent:
            case session_state::logout_replied:
                if (now.mono_ns - logout_at_ >= cfg_.logout_timeout_ns) {
                    sink_.log(session_log::warning, "no Logout or disconnect in time", 0);
                    disconnect();
                }
                break;
            case session_state::disconnected:
                break;
        }
    }

    // An application message: its MsgType and body fields, already encoded as
    // "tag=value" SOH ... (no header or trailer). Sent at once when logged on;
    // otherwise queued unnumbered, sent after logon and recovery (D2).
    send_result send(std::string_view msg_type, std::string_view body, session_time now) {
        if (state_ == session_state::active) {
            emit(msg_type, now, [&](encoder& e) { e.add_raw(body); });
            return send_result::sent;
        }
        if (state_ == session_state::logout_sent || state_ == session_state::logout_replied) return send_result::refused;
        if (queue_count_ == queue_.size() || body.size() > cfg_.max_body || msg_type.size() > sizeof(slot::type))
            return send_result::refused;
        slot& s = queue_[(queue_head_ + queue_count_) % queue_.size()];
        std::memcpy(s.type, msg_type.data(), msg_type.size());
        s.type_length = static_cast<std::uint8_t>(msg_type.size());
        s.body_length = static_cast<std::uint32_t>(body.size());
        std::memcpy(slot_bytes(s), body.data(), body.size());
        ++queue_count_;
        return send_result::queued;
    }

    // Starts the logout process (scenario 12).
    void logout(std::string_view why, session_time now) {
        if (state_ != session_state::active && state_ != session_state::resending) return;
        sync_logout_ = false;
        emit("5", now, [&](encoder& e) {
            if (!why.empty()) e.add(58, why);
        });
        state_ = session_state::logout_sent;
        logout_at_ = now.mono_ns;
    }

    // A logout once the counterparty is in step (note 007): a TestRequest
    // first, and the Logout when its Heartbeat comes back, so the counterparty
    // has processed everything sent before it (it holds anything above a gap
    // until the gap is filled; scenario 16b's check). Only when logged on.
    void logout_when_synced(session_time now) {
        if (state_ != session_state::active && state_ != session_state::resending) return;
        sync_logout_ = true;
        emit("1", now, [&](encoder& e) { e.add(112, sync_id); });
    }
    // A logout asked for and not yet answered (synced or not).
    bool logging_out() const noexcept { return sync_logout_ || state_ == session_state::logout_sent; }

private:
    struct slot {
        char type[3];
        std::uint8_t type_length;
        std::uint32_t body_length;
    };
    char* slot_bytes(const slot& s) noexcept {
        return queue_bytes_.data() + static_cast<std::size_t>(&s - queue_.data()) * cfg_.max_body;
    }

    // A frame starting 8=FIX. whose BeginString (up to its SOH) is not FIX.4.4;
    // a FIX.4.4 frame garbled after it is not (scenario 2t, not 2i).
    static bool wrong_version(const char* p, std::size_t n) noexcept {
        constexpr std::string_view fix = "8=FIX.", ours = "8=FIX.4.4\x01";
        if (n < fix.size() || std::string_view(p, fix.size()) != fix) return false;
        const std::size_t end = std::string_view(p, n).find(soh);
        if (end == std::string_view::npos) return n >= ours.size();  // too long for ours
        return std::string_view(p, end + 1) != ours;
    }

    // MsgSeqNum read straight from the bytes, for a message that did not decode.
    static std::optional<std::uint64_t> raw_seq(const char* p, std::size_t n) noexcept {
        const std::string_view m(p, n);
        const std::size_t at = m.find("\x01" "34=");
        if (at == std::string_view::npos) return std::nullopt;
        const std::size_t end = m.find('\x01', at + 4);
        if (end == std::string_view::npos) return std::nullopt;
        return detail::parse_uint(m.substr(at + 4, end - at - 4));
    }

    void reset_sequences() {
        next_out_ = expected_ = 1;
        store_.reset();
    }

    static constexpr std::string_view sync_id = "LOGOUT-SYNC";

    void disconnect() {
        sink_.disconnect();
        state_ = session_state::disconnected;
        test_pending_ = false;
        gap_target_ = 0;
    }

    void logout_and_disconnect(std::string_view why, session_time now) {
        sink_.log(session_log::error, why, 0);
        emit("5", now, [&](encoder& e) { e.add(58, why); });
        disconnect();
    }

    // Encodes and writes a message with the next outbound number, records it,
    // and advances the number.
    template <class Body>
    void emit(std::string_view msg_type, session_time now, Body&& body) {
        encoder e(out_.data(), out_.size());
        e.begin(msg_type);
        header(e, next_out_, now);
        body(e);
        const std::string_view m = e.finish();
        if (m.empty()) {
            sink_.log(session_log::error, "outbound message larger than max_frame; not sent", next_out_);
            return;
        }
        store_.record(next_out_, m);
        sink_.write(m);
        ++next_out_;
        last_sent_ = now.mono_ns;
    }

    void header(encoder& e, std::uint64_t seq, session_time now) {
        char ts[utc_timestamp_length];
        format_utc_timestamp(now.utc_ms, ts);
        e.add(49, cfg_.sender_comp_id);
        e.add(56, cfg_.target_comp_id);
        e.add(34, seq);
        e.add(52, std::string_view(ts, sizeof(ts)));
    }

    void send_logon(session_time now, std::uint32_t heartbeat_s, bool reset) {
        emit("A", now, [&](encoder& e) {
            e.add(98, std::uint64_t{0});
            e.add(108, std::uint64_t{heartbeat_s});
            if (reset) e.add(141, 'Y');
        });
    }

    void send_reject(std::uint64_t ref_seq, std::uint32_t ref_tag, std::string_view ref_type, int reason,
                     std::string_view why, session_time now) {
        sink_.log(session_log::error, why, ref_seq);
        emit("3", now, [&](encoder& e) {
            e.add(45, ref_seq);
            if (ref_tag != 0) e.add(371, std::uint64_t{ref_tag});
            if (!ref_type.empty()) e.add(372, ref_type);
            e.add(373, static_cast<std::uint64_t>(reason));
            e.add(58, why);
        });
    }

    void on_message(const char* p, std::size_t n, session_time now) {
        const decode_result d = decoder<256>::decode(p, n, in_);
        const std::optional<std::uint64_t> seq =
            d.ok ? detail::parse_uint(in_.get(34).value_or(std::string_view{})) : raw_seq(p, n);
        if (!seq) {
            sink_.log(session_log::warning, "no valid MsgSeqNum: ignored", 0);
            return;
        }
        const std::string_view type = in_.size() >= 3 ? in_.msg_type() : std::string_view{};

        if (state_ == session_state::awaiting_logon || state_ == session_state::logon_sent) {
            if (!d.ok || type != "A") {
                first_not_logon(now);
                return;
            }
            on_logon(*seq, now);
            return;
        }

        if (!d.ok) {  // invalid, scenario 14: Reject, the number consumed
            if (sequence_handled(*seq, now)) return;
            ++expected_;
            send_reject(*seq, d.ref_tag, type, d.reject_reason, d.why, now);
            after_in_order(now);
            return;
        }

        // Header checks that end the session (scenarios 2k, 2o).
        if (in_.get(49) != cfg_.target_comp_id || in_.get(56) != cfg_.sender_comp_id) {
            if (*seq == expected_) ++expected_;
            send_reject(*seq, in_.get(49) != cfg_.target_comp_id ? 49 : 56, type, 9, "CompID problem", now);
            logout_and_disconnect("CompID problem", now);
            return;
        }
        const auto sending = detail::parse_utc_timestamp(in_.get(52).value_or(std::string_view{}));
        if (!sending || *sending - now.utc_ms > cfg_.sending_time_tolerance_ms ||
            now.utc_ms - *sending > cfg_.sending_time_tolerance_ms) {
            if (*seq == expected_) ++expected_;
            send_reject(*seq, 52, type, 10, "SendingTime accuracy problem", now);
            logout_and_disconnect("SendingTime accuracy problem", now);
            return;
        }

        const bool poss_dup = in_.get(43) == std::string_view("Y");
        // Scenarios 2q, 2r and 14 (note 006, D3, D4): a message that fails the
        // dictionary is rejected and its number consumed, under the same
        // sequence rules as a message the codec rejected.
        if (const content c = check_content(type); !c.r.ok) {
            if (*seq < expected_ && poss_dup) {
                sink_.log(session_log::info, "possible duplicate already received: ignored", *seq);
                return;
            }
            if (sequence_handled(*seq, now)) return;
            ++expected_;
            if (c.business) send_business_reject(*seq, type, c.r, now);
            else send_reject(*seq, c.r.ref_tag, type, c.r.reject_reason, c.r.why, now);
            after_in_order(now);
            return;
        }
        if (type == "4" && in_.get(123) != std::string_view("Y")) {  // SequenceReset-Reset: any number
            on_sequence_reset(*seq, now);
            return;
        }
        if (*seq < expected_) {
            if (poss_dup) {
                sink_.log(session_log::info, "possible duplicate already received: ignored", *seq);
                return;
            }
            detail::text why;
            why << "MsgSeqNum too low, expecting " << expected_ << " but received " << *seq;
            logout_and_disconnect(why.view(), now);
            return;
        }
        if (poss_dup) {  // scenarios 2f, 2g
            const auto orig = in_.get(122);
            if (!orig) {
                if (*seq == expected_) ++expected_;
                send_reject(*seq, 122, type, 1, "Required tag missing: OrigSendingTime", now);
                return;
            }
            const auto o = detail::parse_utc_timestamp(*orig);
            if (!o || *o > *sending) {
                if (*seq == expected_) ++expected_;
                send_reject(*seq, 122, type, 10, "OrigSendingTime after SendingTime", now);
                return;
            }
        }
        if (*seq > expected_) {
            if (type == "2") serve_resend(*seq, now);  // served even above a gap (scenario 20)
            if (type == "5") {
                on_logout(now);
                return;
            }
            gap(*seq, now);
            return;
        }
        ++expected_;
        dispatch(*seq, type, now);
        after_in_order(now);
    }

    struct content {
        decode_result r;
        bool business;  // answered with BusinessMessageReject, not Reject
    };
    // The dictionary's verdict on the message in in_ (D3, D4).
    content check_content(std::string_view type) const noexcept {
        if (detail::is_admin(type)) return {validate(in_, type), false};
        if (listed(cfg_.app_msg_types, type)) {
            if (!has_dictionary(type)) return {{true, 0, 0, nullptr}, false};  // a user-defined type the application takes
            if (const decode_result r = validate(in_, type); !r.ok) return {r, false};
            const decode_result c = conditional_rules(in_, type);
            return {c, !c.ok};
        }
        if (!valid_msg_type(type)) return {{false, reject_reason::invalid_msg_type, 35, "Invalid MsgType"}, false};
        return {{false, 0, 0, "Unsupported Message Type"}, true};
    }

    // Scenarios 2r and 14m: BusinessMessageReject, reason 3 (unsupported
    // MsgType) or 5 (a conditionally required field missing, RefTagID... in
    // Text, as BusinessMessageReject has no RefTagID); an application message,
    // so resent (not gap-filled) when asked for.
    void send_business_reject(std::uint64_t ref_seq, std::string_view ref_type, const decode_result& r, session_time now) {
        const bool unsupported = r.reject_reason == 0;
        detail::text why;
        if (unsupported) why << "Unsupported Message Type";
        else why << "Conditionally required field missing: " << std::uint64_t{r.ref_tag};
        sink_.log(session_log::warning, why.view(), ref_seq);
        emit("j", now, [&](encoder& e) {
            e.add(45, ref_seq);
            e.add(372, ref_type);
            e.add(380, std::uint64_t{unsupported ? 3u : static_cast<unsigned>(r.reject_reason)});
            e.add(58, why.view());
        });
    }

    // For an invalid message: the sequence rules that apply before a Reject.
    // True if they dealt with it.
    bool sequence_handled(std::uint64_t seq, session_time now) {
        if (seq < expected_) {
            detail::text why;
            why << "MsgSeqNum too low, expecting " << expected_ << " but received " << seq;
            logout_and_disconnect(why.view(), now);
            return true;
        }
        if (seq > expected_) {
            gap(seq, now);
            return true;
        }
        return false;
    }

    void first_not_logon(session_time now) {
        if (cfg_.role == session_role::acceptor) {  // scenario 1S: log and disconnect
            sink_.log(session_log::error, "first message not a logon", 0);
            disconnect();
            return;
        }
        logout_and_disconnect("first message not a logon", now);  // scenario 1B e
    }

    void on_logon(std::uint64_t seq, session_time now) {
        const bool ids = in_.get(49) == cfg_.target_comp_id && in_.get(56) == cfg_.sender_comp_id;
        if (!ids) {
            if (cfg_.role == session_role::acceptor) {  // scenario 1S c: no message
                sink_.log(session_log::error, "Logon from an unknown CompID", seq);
                disconnect();
            } else {
                logout_and_disconnect("Logon reply with the wrong CompIDs", now);
            }
            return;
        }
        if (const decode_result c = validate(in_, "A"); !c.ok) {  // scenarios 1B d, 1S d
            detail::text why;
            why << "invalid Logon: " << std::string_view(c.why) << ", tag " << std::uint64_t{c.ref_tag};
            logout_and_disconnect(why.view(), now);
            return;
        }
        const auto hb = detail::parse_uint(in_.get(108).value_or(std::string_view{}));
        if (in_.get(98) != std::string_view("0") || !hb || *hb == 0 || *hb > 3600) {
            logout_and_disconnect("invalid Logon: EncryptMethod must be 0 and HeartBtInt 1-3600", now);
            return;
        }
        const auto sending = detail::parse_utc_timestamp(in_.get(52).value_or(std::string_view{}));
        if (!sending || *sending - now.utc_ms > cfg_.sending_time_tolerance_ms ||
            now.utc_ms - *sending > cfg_.sending_time_tolerance_ms) {
            logout_and_disconnect("invalid Logon: SendingTime accuracy problem", now);
            return;
        }
        const bool reset = in_.get(141) == std::string_view("Y");
        if (reset) {
            if (seq != 1) {
                logout_and_disconnect("invalid Logon: ResetSeqNumFlag with MsgSeqNum other than 1", now);
                return;
            }
            if (cfg_.role == session_role::acceptor) reset_sequences();
            else expected_ = 1;  // we asked, or they did: their numbers restart either way
        }
        if (seq < expected_) {
            detail::text why;
            why << "MsgSeqNum too low, expecting " << expected_ << " but received " << seq;
            logout_and_disconnect(why.view(), now);
            return;
        }
        heartbeat_ns_ = static_cast<std::int64_t>(*hb) * 1'000'000'000;
        if (cfg_.role == session_role::acceptor) send_logon(now, static_cast<std::uint32_t>(*hb), reset);
        state_ = session_state::active;
        sink_.log(session_log::info, "logged on", seq);
        if (seq > expected_) {
            gap(seq, now);  // scenarios 1S a, 1B c
            return;
        }
        ++expected_;
        flush_queue(now);
    }

    void dispatch(std::uint64_t seq, std::string_view type, session_time now) {
        if (type.size() == 1) switch (type[0]) {
                case '0':  // Heartbeat
                    if (sync_logout_ && in_.get(112) == std::string_view(sync_id)) logout("", now);
                    return;
                case '1': {  // TestRequest
                    const auto id = in_.get(112);
                    if (!id) {
                        send_reject(seq, 112, type, 1, "Required tag missing: TestReqID", now);
                        return;
                    }
                    emit("0", now, [&](encoder& e) { e.add(112, *id); });
                    return;
                }
                case '2':
                    serve_resend(seq, now);
                    return;
                case '3':
                    sink_.log(session_log::warning, "Reject received", seq);
                    return;
                case '4': {  // SequenceReset-GapFill
                    const auto to = detail::parse_uint(in_.get(36).value_or(std::string_view{}));
                    if (!to) {
                        send_reject(seq, 36, type, 1, "Required tag missing: NewSeqNo", now);
                        return;
                    }
                    if (*to <= seq) {  // scenario 10e
                        send_reject(seq, 36, type, 5, "attempt to lower sequence number, invalid value NewSeqNo", now);
                        return;
                    }
                    expected_ = *to;
                    return;
                }
                case '5':
                    on_logout(now);
                    return;
                case 'A':
                    logout_and_disconnect("Logon received while logged on", now);
                    return;
            }
        sink_.deliver(in_);
    }

    // Scenario 11: SequenceReset-Reset, accepted whatever its MsgSeqNum.
    void on_sequence_reset(std::uint64_t seq, session_time now) {
        const auto to = detail::parse_uint(in_.get(36).value_or(std::string_view{}));
        if (!to) {
            send_reject(seq, 36, "4", 1, "Required tag missing: NewSeqNo", now);
            return;
        }
        if (*to < expected_) {
            send_reject(seq, 36, "4", 5, "SequenceReset would lower the expected MsgSeqNum", now);
            return;
        }
        if (*to == expected_) sink_.log(session_log::warning, "SequenceReset to the expected MsgSeqNum", seq);
        expected_ = *to;
        after_in_order(now);
    }

    void on_logout(session_time now) {
        if (state_ == session_state::logout_sent) {  // scenario 13a
            sink_.log(session_log::info, "logged out", 0);
            disconnect();
            return;
        }
        emit("5", now, [](encoder&) {});  // scenario 13b
        state_ = session_state::logout_replied;
        logout_at_ = now.mono_ns;
    }

    // D1: a message above a gap is dropped; one ResendRequest asks for
    // everything from the gap on.
    void gap(std::uint64_t seq, session_time now) {
        if (seq > gap_target_) gap_target_ = seq;
        if (state_ == session_state::resending) return;
        sink_.log(session_log::info, "MsgSeqNum too high: ResendRequest", seq);
        state_ = session_state::resending;
        const std::uint64_t from = expected_;
        emit("2", now, [&](encoder& e) {
            e.add(7, from);
            e.add(16, std::uint64_t{0});
        });
    }

    // After a message processed in order: the gap may be closed.
    void after_in_order(session_time now) {
        if (state_ == session_state::resending && expected_ > gap_target_) {
            state_ = session_state::active;
            gap_target_ = 0;
            sink_.log(session_log::info, "gap filled", expected_);
            flush_queue(now);
            // the Heartbeat answering a synced logout's TestRequest may have
            // been in our gap, gap-filled: ask again
            if (sync_logout_) emit("1", now, [&](encoder& e) { e.add(112, sync_id); });
        }
    }

    void flush_queue(session_time now) {
        while (queue_count_ > 0 && state_ == session_state::active) {
            const slot& s = queue_[queue_head_];
            const std::string_view type(s.type, s.type_length), body(slot_bytes(s), s.body_length);
            if (sink_.release(type, body)) emit(type, now, [&](encoder& e) { e.add_raw(body); });
            queue_head_ = (queue_head_ + 1) % queue_.size();
            --queue_count_;
        }
    }

    // Scenario 8: application messages and Reject resent with PossDupFlag=Y
    // (BusinessMessageReject always, the others as the sink's hook decides);
    // the rest, and anything the store no longer has, gap-filled.
    void serve_resend(std::uint64_t seq, session_time now) {
        const auto begin = detail::parse_uint(in_.get(7).value_or(std::string_view{}));
        const auto end_requested = detail::parse_uint(in_.get(16).value_or(std::string_view{}));
        if (!begin || !end_requested || *begin == 0) {
            send_reject(seq, begin ? 16 : 7, "2", 1, "ResendRequest needs BeginSeqNo and EndSeqNo", now);
            return;
        }
        const std::uint64_t last = next_out_ - 1;
        const std::uint64_t end = (*end_requested == 0 || *end_requested > last) ? last : *end_requested;
        if (*begin > end) {
            sink_.log(session_log::warning, "ResendRequest for nothing we sent", seq);
            return;
        }
        std::uint64_t fill_from = 0;
        for (std::uint64_t s = *begin; s <= end; ++s) {
            const std::string_view bytes = store_.lookup(s);
            bool resend = false;
            if (!bytes.empty() && decoder<256>::decode(bytes.data(), bytes.size(), stored_).ok) {
                const std::string_view t = stored_.msg_type();
                resend = t == "3" || t == "j" || (!detail::is_admin(t) && sink_.resend(stored_));
            }
            if (!resend) {
                if (fill_from == 0) fill_from = s;
                continue;
            }
            if (fill_from != 0) {
                gap_fill(fill_from, s, now);
                fill_from = 0;
            }
            resend_stored(now);
        }
        if (fill_from != 0) gap_fill(fill_from, end + 1, now);
        // A synced logout's TestRequest in the range was gap-filled (an admin
        // message): ask again, now the counterparty is catching up.
        if (sync_logout_) emit("1", now, [&](encoder& e) { e.add(112, sync_id); });
    }

    void gap_fill(std::uint64_t from, std::uint64_t to, session_time now) {
        encoder e(out_.data(), out_.size());
        e.begin("4");
        header(e, from, now);
        char ts[utc_timestamp_length];
        format_utc_timestamp(now.utc_ms, ts);
        e.add(43, 'Y');
        e.add(122, std::string_view(ts, sizeof(ts)));
        e.add(123, 'Y');
        e.add(36, to);
        write_replay(e);
    }

    void resend_stored(session_time now) {
        encoder e(out_.data(), out_.size());
        e.begin(stored_.msg_type());
        header(e, detail::parse_uint(stored_.get(34).value_or(std::string_view{})).value_or(0), now);
        e.add(43, 'Y');
        e.add(122, stored_.get(52).value_or(std::string_view{}));
        for (std::size_t i = 0; i < stored_.size(); ++i) {
            const std::uint32_t t = stored_[i].tag;
            if (detail::is_header_tag(t) || detail::is_trailer_tag(t)) continue;
            e.add(t, stored_.value(i));
        }
        write_replay(e);
    }

    // A resent message or gap fill: written, not recorded, numbers unchanged.
    void write_replay(encoder& e) {
        const std::string_view m = e.finish();
        if (m.empty()) {
            sink_.log(session_log::error, "resent message larger than max_frame; not sent", 0);
            return;
        }
        sink_.write(m);
    }

    const session_config cfg_;
    Sink& sink_;
    Store& store_;
    std::vector<char> out_;
    message_view<256> in_, stored_;
    session_state state_ = session_state::disconnected;
    std::uint64_t next_out_ = 1, expected_ = 1, gap_target_ = 0, test_id_ = 0;
    std::int64_t heartbeat_ns_ = 30'000'000'000;
    std::int64_t connected_at_ = 0, last_sent_ = 0, last_recv_ = 0, test_sent_at_ = 0, logout_at_ = 0;
    bool test_pending_ = false, sync_logout_ = false;
    std::vector<slot> queue_;
    std::vector<char> queue_bytes_;
    std::size_t queue_head_ = 0, queue_count_ = 0;
};

}  // namespace fixgw
