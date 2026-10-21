// Framing a byte stream into FIX 4.4 messages. Design: docs/design/002-codec.md
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

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

// Whether this build has the SIMD paths (note 009): x86-64-v3 or later.
#if defined(__AVX2__)
inline constexpr bool simd_available = true;
#else
inline constexpr bool simd_available = false;
#endif

// The sum of `n` bytes modulo 256: FIX's CheckSum, a byte at a time.
inline unsigned checksum_scalar(const char* p, std::size_t n) noexcept {
    unsigned sum = 0;
    for (std::size_t i = 0; i < n; ++i) sum += static_cast<unsigned char>(p[i]);
    return sum & 0xffU;
}

// The same, 32 bytes per step (note 009): vpsadbw against zero sums each
// 8-byte lane into a 64-bit total. Whole blocks only, the rest a byte at a
// time, so it never reads past `n`.
inline unsigned checksum_simd(const char* p, std::size_t n) noexcept {
#if defined(__AVX2__)
    __m256i acc = _mm256_setzero_si256();
    const __m256i zero = _mm256_setzero_si256();
    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
        acc = _mm256_add_epi64(acc, _mm256_sad_epu8(v, zero));
    }
    const __m128i s = _mm_add_epi64(_mm256_castsi256_si128(acc), _mm256_extracti128_si256(acc, 1));
    unsigned sum = static_cast<unsigned>(_mm_cvtsi128_si64(s) + _mm_extract_epi64(s, 1));
    for (; i < n; ++i) sum += static_cast<unsigned char>(p[i]);
    return sum & 0xffU;
#else
    return checksum_scalar(p, n);
#endif
}

// The default: SIMD where the build has it.
inline unsigned checksum(const char* p, std::size_t n) noexcept {
    return simd_available ? checksum_simd(p, n) : checksum_scalar(p, n);
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
// `Simd` chooses the CheckSum; everything else is the same code.
template <bool Simd>
inline frame_result frame_with(const char* p, std::size_t n, std::size_t max_frame) noexcept {
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
    const unsigned sum = Simd ? checksum_simd(p, body + body_length) : checksum_scalar(p, body + body_length);
    if (stated != sum) return {frame_status::garbled, total, "CheckSum is wrong"};
    return {frame_status::complete, total, nullptr};
}

inline frame_result frame_scalar(const char* p, std::size_t n, std::size_t max_frame) noexcept {
    return frame_with<false>(p, n, max_frame);
}
inline frame_result frame_simd(const char* p, std::size_t n, std::size_t max_frame) noexcept {
    return frame_with<true>(p, n, max_frame);
}
// The default: SIMD where the build has it.
inline frame_result frame(const char* p, std::size_t n, std::size_t max_frame) noexcept {
    return frame_with<simd_available>(p, n, max_frame);
}

}  // namespace fixgw
