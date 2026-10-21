// Decoding a framed FIX 4.4 message into views over its bytes (representation
// B). Design: docs/design/002-codec.md
#pragma once

#include <fixgw/framing.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace fixgw {

// SessionRejectReason (373) values the codec can establish on its own.
namespace reject_reason {
inline constexpr int invalid_tag_number = 0;
inline constexpr int tag_without_value = 4;
inline constexpr int incorrect_data_format = 6;
inline constexpr int tag_repeated = 13;
inline constexpr int tag_out_of_order = 14;
inline constexpr int other = 99;
}  // namespace reject_reason

// One field: its tag and where its value lies in the message.
struct field {
    std::uint32_t tag;
    std::uint32_t offset, length;
};

// A message as views over the frame it was decoded from: valid while those
// bytes are. Fields in order, repeated tags included (repeating groups are
// returned as they come, not interpreted).
template <std::size_t MaxFields = 256>
class message_view {
public:
    std::size_t size() const noexcept { return count_; }
    const field& operator[](std::size_t i) const noexcept { return fields_[i]; }
    std::string_view value(const field& f) const noexcept { return {base_ + f.offset, f.length}; }
    std::string_view value(std::size_t i) const noexcept { return value(fields_[i]); }

    // The first occurrence of `tag`, if any.
    std::optional<std::string_view> get(std::uint32_t tag) const noexcept {
        for (std::size_t i = 0; i < count_; ++i)
            if (fields_[i].tag == tag) return value(fields_[i]);
        return std::nullopt;
    }
    std::string_view msg_type() const noexcept { return value(fields_[2]); }  // framing put 35 third
    std::string_view bytes() const noexcept { return {base_, length_}; }

private:
    template <std::size_t N, class S>
    friend struct decoder;
    const char* base_ = nullptr;
    std::size_t length_ = 0, count_ = 0;
    field fields_[MaxFields];
};

struct decode_result {
    bool ok;
    int reject_reason;     // when !ok: SessionRejectReason (373)
    std::uint32_t ref_tag;  // when !ok: the tag concerned, 0 if none (RefTagID, 371)
    const char* why;
};

namespace detail {

// FIX 4.4 header and trailer tags. The header comes first, the trailer last;
// a header or trailer tag elsewhere is out of order, and one repeated is an
// error, except the NoHops group's fields (627-630).
inline constexpr bool is_header_tag(std::uint32_t t) noexcept {
    switch (t) {
        case 8: case 9: case 35: case 49: case 56: case 115: case 128: case 90: case 91: case 34:
        case 50: case 142: case 57: case 143: case 116: case 144: case 129: case 145: case 43:
        case 97: case 52: case 122: case 212: case 213: case 347: case 369: case 627: case 628:
        case 629: case 630:
            return true;
        default:
            return false;
    }
}
inline constexpr bool is_trailer_tag(std::uint32_t t) noexcept { return t == 93 || t == 89 || t == 10; }
inline constexpr bool is_group_tag(std::uint32_t t) noexcept { return t >= 628 && t <= 630; }

// A distinct bit for each non-group header tag (checked by the tests).
inline constexpr unsigned header_index(std::uint32_t t) noexcept {
    switch (t) {
        case 8: return 0; case 9: return 1; case 35: return 2; case 49: return 3; case 56: return 4;
        case 115: return 5; case 128: return 6; case 90: return 7; case 91: return 8; case 34: return 9;
        case 50: return 10; case 142: return 11; case 57: return 12; case 143: return 13; case 116: return 14;
        case 144: return 15; case 129: return 16; case 145: return 17; case 43: return 18; case 97: return 19;
        case 52: return 20; case 122: return 21; case 212: return 22; case 213: return 23; case 347: return 24;
        case 369: return 25; case 627: return 26;
        default: return 63;
    }
}

// Data fields: the length tag, and the tag whose value is that many raw bytes
// (which may contain SOH). 0 if `t` is not a length tag.
inline constexpr std::uint32_t data_tag_for(std::uint32_t t) noexcept {
    switch (t) {
        case 90: return 91;    // SecureDataLen, SecureData
        case 212: return 213;  // XmlDataLen, XmlData
        case 93: return 89;    // SignatureLength, Signature
        case 95: return 96;    // RawDataLength, RawData
        default: return 0;
    }
}

}  // namespace detail

// How the decoder finds the SOH that ends a value (note 009). Each returns the
// first SOH at or after `i` in p[0, n), or n if there is none.
namespace scan {

// A byte at a time: note 002's decoder.
struct scalar {
    static std::size_t find_soh(const char* p, std::size_t i, std::size_t n) noexcept {
        while (i < n && p[i] != soh) ++i;
        return i;
    }
};

// 32 bytes at a time: compare with SOH, take the mask, count trailing zeros.
// `Padded`: the caller guarantees at least 32 readable bytes past p + n (the
// receive buffer's padding), so every load is a full one and bits past n are
// masked off. Otherwise whole blocks only, and the last few bytes one at a time.
template <bool Padded>
struct simd {
    static std::size_t find_soh(const char* p, std::size_t i, std::size_t n) noexcept {
#if defined(__AVX2__)
        const __m256i s = _mm256_set1_epi8(soh);
        for (; Padded ? i < n : i + 32 <= n; i += 32) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
            auto mask = static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, s)));
            if constexpr (Padded)
                if (n - i < 32) mask &= (std::uint32_t{1} << (n - i)) - 1;
            if (mask) return i + static_cast<std::size_t>(_tzcnt_u32(mask));
        }
        if constexpr (Padded) return n;
#endif
        return scalar::find_soh(p, i, n);
    }
};

// The default: SIMD where the build has it, unpadded (safe for any view).
using best = std::conditional_t<simd_available, simd<false>, scalar>;

}  // namespace scan

template <std::size_t MaxFields = 256, class Scan = scan::best>
struct decoder {
    // Decodes a frame that `frame()` reported complete. One pass, no copies,
    // no allocation: each field becomes (tag, offset, length).
    static decode_result decode(const char* p, std::size_t n, message_view<MaxFields>& out) noexcept {
        out.base_ = p;
        out.length_ = n;
        out.count_ = 0;
        enum { header, body, trailer } part = header;
        std::uint64_t seen_header = 0;  // which header tags have appeared, by a small index
        std::size_t data_length = 0;    // set by a length tag; the next field must be its data
        std::uint32_t data_tag = 0;
        std::size_t i = 0;
        while (i < n) {
            // Tag: a positive integer without a leading zero.
            std::uint32_t tag = 0;
            const std::size_t tag_start = i;
            while (i < n && p[i] >= '0' && p[i] <= '9') {
                if (tag > 99'999'999) return fail(reject_reason::invalid_tag_number, 0, "tag too large");
                tag = tag * 10 + static_cast<std::uint32_t>(p[i] - '0');
                ++i;
            }
            if (i == tag_start || i == n || p[i] != '=' || p[tag_start] == '0')
                return fail(reject_reason::invalid_tag_number, 0, "tag is not a positive integer followed by =");
            ++i;
            // Value: by length for a data field, else to the next SOH.
            std::size_t value_start = i, value_length;
            if (data_tag != 0) {
                if (tag != data_tag)
                    return fail(reject_reason::incorrect_data_format, tag, "a data field does not follow its length");
                if (n - i < data_length + 1 || p[i + data_length] != soh)
                    return fail(reject_reason::incorrect_data_format, tag, "data field shorter than its length");
                value_length = data_length;
                i += data_length;
                data_tag = 0;
            } else {
                i = Scan::find_soh(p, i, n);
                if (i == n) return fail(reject_reason::other, tag, "field not ended by SOH");
                value_length = i - value_start;
            }
            if (value_length == 0) return fail(reject_reason::tag_without_value, tag, "tag without a value");
            ++i;  // past SOH

            // Placement: header first, then body, then trailer.
            if (detail::is_header_tag(tag)) {
                if (part != header) return fail(reject_reason::tag_out_of_order, tag, "header tag after the body");
                if (!detail::is_group_tag(tag)) {
                    const std::uint64_t bit = std::uint64_t{1} << detail::header_index(tag);
                    if (seen_header & bit) return fail(reject_reason::tag_repeated, tag, "header tag repeated");
                    seen_header |= bit;
                }
            } else if (detail::is_trailer_tag(tag)) {
                if (part == trailer && tag != 10 && tag != 89)
                    return fail(reject_reason::tag_repeated, tag, "trailer tag repeated");
                part = trailer;
            } else {
                if (part == trailer) return fail(reject_reason::tag_out_of_order, tag, "body tag after the trailer");
                part = body;
            }
            if (const std::uint32_t d = detail::data_tag_for(tag)) {
                std::size_t len = 0;
                for (std::size_t k = value_start; k < value_start + value_length; ++k) {
                    if (p[k] < '0' || p[k] > '9' || len > 99'999'999)
                        return fail(reject_reason::incorrect_data_format, tag, "data length is not a number");
                    len = len * 10 + static_cast<std::size_t>(p[k] - '0');
                }
                data_tag = d;
                data_length = len;
            }
            if (out.count_ == MaxFields) return fail(reject_reason::other, tag, "more fields than the decoder holds");
            out.fields_[out.count_++] = {tag, static_cast<std::uint32_t>(value_start),
                                         static_cast<std::uint32_t>(value_length)};
        }
        if (data_tag != 0) return fail(reject_reason::incorrect_data_format, data_tag, "data length with no data field");
        // Framing found "10=" where BodyLength put it, but a data length can
        // still reach over it; the trailer must be the last field decoded.
        if (out.count_ == 0 || out.fields_[out.count_ - 1].tag != 10)
            return fail(reject_reason::incorrect_data_format, 0, "a data field reaches into the trailer");
        return {true, 0, 0, nullptr};
    }

private:
    static decode_result fail(int reason, std::uint32_t tag, const char* why) noexcept {
        return {false, reason, tag, why};
    }
};

// Decodes with the default capacity.
inline decode_result decode(const char* p, std::size_t n, message_view<>& out) noexcept {
    return decoder<>::decode(p, n, out);
}

}  // namespace fixgw
