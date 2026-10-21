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

struct decode_result {
    bool ok;
    int reject_reason;     // when !ok: SessionRejectReason (373)
    std::uint32_t ref_tag;  // when !ok: the tag concerned, 0 if none (RefTagID, 371)
    const char* why;
};

template <std::size_t Words>
struct structural_index;

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
    template <std::size_t N, std::size_t W>
    friend decode_result decode_indexed(const char*, const structural_index<W>&, std::size_t, std::size_t,
                                        message_view<N>&) noexcept;
    const char* base_ = nullptr;
    std::size_t length_ = 0, count_ = 0;
    field fields_[MaxFields];
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
    [[gnu::always_inline]] static std::size_t find_soh(const char* p, std::size_t i, std::size_t n) noexcept {
        FIXGW_NOVECTOR
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
    [[gnu::always_inline]] static std::size_t find_soh(const char* p, std::size_t i, std::size_t n) noexcept {
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

namespace detail {

// The rules every decoder applies to each field, whatever found it: header,
// body and trailer in order; header tags not repeated (the NoHops group
// excepted); a data field's length read and its data checked; the field
// stored. The decoders differ only in how they find '=' and SOH (note 009), so
// they agree by construction, and the tests check it.
template <std::size_t MaxFields>
struct field_rules {
    enum { header, body, trailer } part = header;
    std::uint64_t seen_header = 0;
    std::size_t data_length = 0;  // set by a length tag; the next field must be its data
    std::uint32_t data_tag = 0;

    static decode_result fail(int reason, std::uint32_t tag, const char* why) noexcept {
        return {false, reason, tag, why};
    }

    // One field whose value is p[value_start, value_start + value_length).
    // `count` is the caller's local, so it stays in a register.
    [[gnu::always_inline]] decode_result add(const char* p, std::uint32_t tag, std::size_t value_start, std::size_t value_length,
                      std::size_t& count, field* fields) noexcept {
        if (value_length == 0) return fail(reject_reason::tag_without_value, tag, "tag without a value");
        if (is_header_tag(tag)) {
            if (part != header) return fail(reject_reason::tag_out_of_order, tag, "header tag after the body");
            if (!is_group_tag(tag)) {
                const std::uint64_t bit = std::uint64_t{1} << header_index(tag);
                if (seen_header & bit) return fail(reject_reason::tag_repeated, tag, "header tag repeated");
                seen_header |= bit;
            }
        } else if (is_trailer_tag(tag)) {
            if (part == trailer && tag != 10 && tag != 89) return fail(reject_reason::tag_repeated, tag, "trailer tag repeated");
            part = trailer;
        } else {
            if (part == trailer) return fail(reject_reason::tag_out_of_order, tag, "body tag after the trailer");
            part = body;
        }
        if (const std::uint32_t d = data_tag_for(tag)) {
            std::size_t len = 0;
            FIXGW_NOVECTOR
            for (std::size_t k = value_start; k < value_start + value_length; ++k) {
                if (p[k] < '0' || p[k] > '9' || len > 99'999'999)
                    return fail(reject_reason::incorrect_data_format, tag, "data length is not a number");
                len = len * 10 + static_cast<std::size_t>(p[k] - '0');
            }
            data_tag = d;
            data_length = len;
        }
        if (count == MaxFields) return fail(reject_reason::other, tag, "more fields than the decoder holds");
        fields[count++] = {tag, static_cast<std::uint32_t>(value_start), static_cast<std::uint32_t>(value_length)};
        return {true, 0, 0, nullptr};
    }

    // After the last field.
    [[gnu::always_inline]] decode_result finish(std::size_t count, const field* fields) const noexcept {
        if (data_tag != 0) return fail(reject_reason::incorrect_data_format, data_tag, "data length with no data field");
        // Framing found "10=" where BodyLength put it, but a data length can
        // still reach over it; the trailer must be the last field decoded.
        if (count == 0 || fields[count - 1].tag != 10)
            return fail(reject_reason::incorrect_data_format, 0, "a data field reaches into the trailer");
        return {true, 0, 0, nullptr};
    }
};

}  // namespace detail

template <std::size_t MaxFields = 256, class Scan = scan::best>
struct decoder {
    // Decodes a frame that `frame()` reported complete. One pass, no copies,
    // no allocation: each field becomes (tag, offset, length). '=' is found
    // by reading the tag's digits; SOH by `Scan`.
    [[gnu::always_inline]] static decode_result decode(const char* p, std::size_t n, message_view<MaxFields>& out) noexcept {
        out.base_ = p;
        out.length_ = n;
        out.count_ = 0;
        detail::field_rules<MaxFields> rules;
        std::size_t count = 0;
        const auto done = [&](const decode_result& r) noexcept {
            out.count_ = count;
            return r;
        };
        std::size_t i = 0;
        while (i < n) {
            // Tag: a positive integer without a leading zero, read and
            // converted in one pass.
            std::uint32_t tag = 0;
            const std::size_t tag_start = i;
            FIXGW_NOVECTOR
            while (i < n && p[i] >= '0' && p[i] <= '9') {
                if (tag > 99'999'999) return done(rules.fail(reject_reason::invalid_tag_number, 0, "tag too large"));
                tag = tag * 10 + static_cast<std::uint32_t>(p[i] - '0');
                ++i;
            }
            if (i == tag_start || i == n || p[i] != '=' || p[tag_start] == '0')
                return done(rules.fail(reject_reason::invalid_tag_number, 0, "tag is not a positive integer followed by ="));
            ++i;
            std::size_t value_start = i, value_length;
            if (rules.data_tag != 0) {
                if (tag != rules.data_tag)
                    return done(rules.fail(reject_reason::incorrect_data_format, tag, "a data field does not follow its length"));
                if (n - i < rules.data_length + 1 || p[i + rules.data_length] != soh)
                    return done(rules.fail(reject_reason::incorrect_data_format, tag, "data field shorter than its length"));
                value_length = rules.data_length;
                i += rules.data_length;
                rules.data_tag = 0;
            } else {
                i = Scan::find_soh(p, i, n);
                if (i == n) return done(rules.fail(reject_reason::other, tag, "field not ended by SOH"));
                value_length = i - value_start;
            }
            ++i;  // past SOH
            const auto r = rules.add(p, tag, value_start, value_length, count, out.fields_);
            if (!r.ok) return done(r);
        }
        return done(rules.finish(count, out.fields_));
    }
};

// Stage one of two-stage decoding (note 009): bitmaps of where every '=' and
// SOH is, one bit per byte, 64 bytes per word, built 64 bytes at a time. Built
// once over a whole receive buffer, it serves every message in it. `Words`
// bounds the buffer: 64 bytes per word.
template <std::size_t Words>
struct structural_index {
    std::uint64_t eq[Words + 1];
    std::uint64_t sep[Words + 1];
    std::size_t n = 0;

    // `Padded`: at least 64 readable bytes past p + n. Otherwise the last
    // partial block is read a byte at a time. False if n exceeds the capacity.
    template <bool Padded = false>
    [[gnu::always_inline]] bool build(const char* p, std::size_t len) noexcept {
        if (len > Words * 64) return false;
        n = len;
        std::size_t i = 0, w = 0;
#if defined(__AVX2__)
        const __m256i e = _mm256_set1_epi8('='), s = _mm256_set1_epi8(soh);
        for (; Padded ? i < len : i + 64 <= len; i += 64, ++w) {
            const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
            const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i + 32));
            std::uint64_t me = static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, e))) |
                               std::uint64_t{static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, e)))} << 32;
            std::uint64_t ms = static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, s))) |
                               std::uint64_t{static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, s)))} << 32;
            if (Padded && len - i < 64) {
                const std::uint64_t keep = (std::uint64_t{1} << (len - i)) - 1;
                me &= keep;
                ms &= keep;
            }
            eq[w] = me;
            sep[w] = ms;
        }
        if (Padded) {
            eq[w] = sep[w] = 0;
            return true;
        }
#endif
        for (; i < len; i += 64, ++w) {
            std::uint64_t me = 0, ms = 0;
            for (std::size_t k = 0; k < 64 && i + k < len; ++k) {
                me |= std::uint64_t{p[i + k] == '='} << k;
                ms |= std::uint64_t{p[i + k] == soh} << k;
            }
            eq[w] = me;
            sep[w] = ms;
        }
        eq[w] = sep[w] = 0;
        return true;
    }

    // The first set bit at or after `pos` and before `end`, or `end`.
    [[gnu::always_inline]] static std::size_t next(const std::uint64_t* bits, std::size_t pos, std::size_t end) noexcept {
        std::size_t w = pos >> 6;
        std::uint64_t m = bits[w] & (~std::uint64_t{0} << (pos & 63));
        while (m == 0) {
            if ((++w << 6) >= end) return end;
            m = bits[w];
        }
        const std::size_t at = (w << 6) + static_cast<std::size_t>(__builtin_ctzll(m));
        return at < end ? at : end;
    }
};

// Stage two: the message at base[off, off + len), whose index `ix` covers it.
// Each field's '=' and SOH come from the bitmaps; the tag's digits are still
// read, to validate and convert them. Field offsets are relative to the message.
template <std::size_t MaxFields = 256, std::size_t Words>
[[gnu::always_inline]] inline decode_result decode_indexed(const char* base, const structural_index<Words>& ix, std::size_t off, std::size_t len,
                             message_view<MaxFields>& out) noexcept {
    const char* p = base + off;
    out.base_ = p;
    out.length_ = len;
    out.count_ = 0;
    detail::field_rules<MaxFields> rules;
    std::size_t count = 0;
    const auto done = [&](const decode_result& r) noexcept {
        out.count_ = count;
        return r;
    };
    const std::size_t end = off + len;
    std::size_t i = off;  // absolute, in the index
    while (i < end) {
        const std::size_t eq = structural_index<Words>::next(ix.eq, i, end);
        // The tag runs to the first non-digit; if that is not the '=', or the
        // tag is empty or starts with 0, it is invalid, as the scalar decoder says.
        std::size_t d = i;
        std::uint32_t tag = 0;
        FIXGW_NOVECTOR
        while (d < eq && base[d] >= '0' && base[d] <= '9') {
            if (tag > 99'999'999) return done(rules.fail(reject_reason::invalid_tag_number, 0, "tag too large"));
            tag = tag * 10 + static_cast<std::uint32_t>(base[d] - '0');
            ++d;
        }
        if (d == i || d != eq || eq == end || base[i] == '0')
            return done(rules.fail(reject_reason::invalid_tag_number, 0, "tag is not a positive integer followed by ="));
        const std::size_t value_start = eq + 1;
        std::size_t value_length;
        if (rules.data_tag != 0) {
            if (tag != rules.data_tag)
                return done(rules.fail(reject_reason::incorrect_data_format, tag, "a data field does not follow its length"));
            if (end - value_start < rules.data_length + 1 || base[value_start + rules.data_length] != soh)
                return done(rules.fail(reject_reason::incorrect_data_format, tag, "data field shorter than its length"));
            value_length = rules.data_length;
            i = value_start + rules.data_length;
            rules.data_tag = 0;
        } else {
            i = structural_index<Words>::next(ix.sep, value_start, end);
            if (i == end) return done(rules.fail(reject_reason::other, tag, "field not ended by SOH"));
            value_length = i - value_start;
        }
        ++i;
        const auto r = rules.add(p, tag, value_start - off, value_length, count, out.fields_);
        if (!r.ok) return done(r);
    }
    return done(rules.finish(count, out.fields_));
}

// Decodes with the default capacity.
inline decode_result decode(const char* p, std::size_t n, message_view<>& out) noexcept {
    return decoder<>::decode(p, n, out);
}

}  // namespace fixgw
