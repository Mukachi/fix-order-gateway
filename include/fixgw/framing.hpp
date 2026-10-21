// Framing a byte stream into FIX 4.4 messages. Design: docs/design/002-codec.md
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fixgw {

inline constexpr char soh = '\x01';
inline constexpr std::string_view begin_string = "8=FIX.4.4\x01" "9=";  // every message starts so
inline constexpr std::size_t max_body_length_digits = 7;                   // a frame under 10 MB
inline constexpr std::size_t trailer_length = 7;                           // "10=ddd" SOH

enum class frame_status {
    complete,   // a whole, correctly framed message of `length` bytes is at the start
    need_more,  // the start is a valid prefix of a message: read more
    garbled,    // discard `length` bytes, then frame again (FIX: ignore the message)
};

struct frame_result {
    frame_status status;
    std::size_t length;  // complete: the frame's bytes; garbled: bytes to discard; need_more: 0
    const char* why;     // garbled: the reason, for logging; otherwise nullptr
};

// The sum of `n` bytes modulo 256: FIX's CheckSum.
inline unsigned checksum(const char* p, std::size_t n) noexcept {
    unsigned sum = 0;
    for (std::size_t i = 0; i < n; ++i) sum += static_cast<unsigned char>(p[i]);
    return sum & 0xffU;
}

namespace detail {

// Where the next message could start, after the first byte: the first
// position from which `begin_string` matches, or matches as far as the buffer
// goes. Everything before it is discarded.
inline std::size_t next_start(const char* p, std::size_t n) noexcept {
    for (std::size_t i = 1; i < n; ++i) {
        if (p[i] != '8') continue;
        std::size_t k = 0;
        while (k < begin_string.size() && i + k < n && p[i + k] == begin_string[k]) ++k;
        if (k == begin_string.size() || i + k == n) return i;
    }
    return n;
}

inline frame_result garbled(const char* p, std::size_t n, const char* why) noexcept {
    return {frame_status::garbled, next_start(p, n), why};
}

}  // namespace detail

// Frames the message at the start of p[0, n). Reads BodyLength and checks
// that "10=ddd" SOH sits exactly where it says, with the right sum; never
// scans a body for its end, so a body cannot mislead it. Also checks that
// MsgType (35) is the first field of the body. `max_frame` bounds a message
// (the receive buffer's size): a frame longer than that is garbled.
inline frame_result frame(const char* p, std::size_t n, std::size_t max_frame) noexcept {
    // BeginString and the start of BodyLength.
    const std::size_t prefix = begin_string.size();
    for (std::size_t i = 0; i < prefix; ++i) {
        if (i == n) return {frame_status::need_more, 0, nullptr};
        if (p[i] != begin_string[i]) return detail::garbled(p, n, "does not start with 8=FIX.4.4, 9=");
    }
    // BodyLength: digits, then SOH.
    std::size_t body_length = 0, i = prefix;
    for (;; ++i) {
        if (i == n) return {frame_status::need_more, 0, nullptr};
        const char c = p[i];
        if (c == soh) break;
        if (c < '0' || c > '9' || i - prefix == max_body_length_digits)
            return detail::garbled(p, n, "BodyLength is not a number");
        body_length = body_length * 10 + static_cast<std::size_t>(c - '0');
    }
    if (i == prefix) return detail::garbled(p, n, "BodyLength is empty");
    const std::size_t body = i + 1;
    const std::size_t total = body + body_length + trailer_length;
    if (total > max_frame) return detail::garbled(p, n, "message longer than the receive buffer");
    // MsgType first in the body, as far as the bytes go.
    for (std::size_t k = 0; k < 3; ++k) {
        if (body + k == n) return {frame_status::need_more, 0, nullptr};
        if (k == body_length || p[body + k] != "35="[k]) return detail::garbled(p, n, "MsgType (35) is not the third field");
    }
    if (n < total) return {frame_status::need_more, 0, nullptr};
    // The trailer exactly where BodyLength puts it.
    const char* t = p + body + body_length;
    if (t[-1] != soh || t[0] != '1' || t[1] != '0' || t[2] != '=' || t[6] != soh)
        return detail::garbled(p, n, "CheckSum (10) is not where BodyLength puts it");
    unsigned stated = 0;
    for (int k = 3; k < 6; ++k) {
        if (t[k] < '0' || t[k] > '9') return {frame_status::garbled, total, "CheckSum is not three digits"};
        stated = stated * 10 + static_cast<unsigned>(t[k] - '0');
    }
    if (stated != checksum(p, body + body_length)) return {frame_status::garbled, total, "CheckSum is wrong"};
    return {frame_status::complete, total, nullptr};
}

}  // namespace fixgw
