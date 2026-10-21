// Encoding FIX 4.4 messages: fields appended to a fixed buffer (B), and
// pre-encoded order templates where only the changing fields are written (C).
// Design: docs/design/002-codec.md
#pragma once

#include <fixgw/framing.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace fixgw {

// A UTCTimestamp with milliseconds, "YYYYMMDD-HH:MM:SS.sss": FIX 4.4 allows
// whole seconds or milliseconds.
inline constexpr std::size_t utc_timestamp_length = 21;

// Writes `unix_ms` (milliseconds since 1970-01-01 UTC) as a UTCTimestamp.
// Days to civil date by Howard Hinnant's public algorithm; no library call.
inline void format_utc_timestamp(std::int64_t unix_ms, char* out) noexcept {
    std::int64_t days = unix_ms / 86'400'000, ms = unix_ms % 86'400'000;
    if (ms < 0) {
        ms += 86'400'000;
        --days;
    }
    const std::int64_t z = days + 719'468;
    const std::int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
    const std::int64_t doe = z - era * 146'097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36'524 - doe / 146'096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t y = yoe + era * 400 + (m <= 2);
    const auto put = [&out](int at, std::int64_t v, int width) {
        for (int i = width - 1; i >= 0; --i) {
            out[at + i] = static_cast<char>('0' + v % 10);
            v /= 10;
        }
    };
    put(0, y, 4);
    put(4, m, 2);
    put(6, d, 2);
    out[8] = '-';
    put(9, ms / 3'600'000, 2);
    out[11] = ':';
    put(12, ms / 60'000 % 60, 2);
    out[14] = ':';
    put(15, ms / 1000 % 60, 2);
    out[17] = '.';
    put(18, ms % 1000, 3);
}

namespace detail {

// Decimal digits of v, written backwards from `end`; returns the start.
inline char* put_uint(char* end, std::uint64_t v) noexcept {
    do {
        *--end = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    return end;
}

// v in exactly `width` digits, with leading zeros. False if it does not fit.
inline bool put_fixed(char* at, std::uint64_t v, int width) noexcept {
    for (int i = width - 1; i >= 0; --i) {
        at[i] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    return v == 0;
}

}  // namespace detail

// Representation B: fields appended to a caller's buffer, BodyLength and
// CheckSum computed at the end. The body is written first, after room for the
// longest prefix; `finish` puts the prefix right before it. No allocation;
// running out of room sets `overflow` and `finish` returns nothing.
class encoder {
public:
    static constexpr std::size_t prefix_room = begin_string.size() + max_body_length_digits + 1;

    encoder(char* buffer, std::size_t capacity) noexcept : buf_(buffer), cap_(capacity) {}

    void begin(std::string_view msg_type) noexcept {
        pos_ = prefix_room;
        overflow_ = cap_ < prefix_room;
        add(35, msg_type);
    }
    void add(std::uint32_t tag, std::string_view value) noexcept {
        char tmp[10];
        char* t = detail::put_uint(tmp + sizeof(tmp), tag);
        const std::size_t tl = static_cast<std::size_t>(tmp + sizeof(tmp) - t);
        if (!room(tl + 1 + value.size() + 1)) return;
        std::memcpy(buf_ + pos_, t, tl);
        pos_ += tl;
        buf_[pos_++] = '=';
        std::memcpy(buf_ + pos_, value.data(), value.size());
        pos_ += value.size();
        buf_[pos_++] = soh;
    }
    void add(std::uint32_t tag, std::uint64_t value) noexcept {
        char tmp[20];
        char* v = detail::put_uint(tmp + sizeof(tmp), value);
        add(tag, std::string_view(v, static_cast<std::size_t>(tmp + sizeof(tmp) - v)));
    }
    void add(std::uint32_t tag, char value) noexcept { add(tag, std::string_view(&value, 1)); }
    // A decimal from an integer count of 10^-decimals: (7253, 2) is "72.53".
    void add_decimal(std::uint32_t tag, std::int64_t mantissa, int decimals) noexcept {
        char tmp[24];
        char* end = tmp + sizeof(tmp);
        const bool negative = mantissa < 0;
        std::uint64_t m = negative ? 0 - static_cast<std::uint64_t>(mantissa) : static_cast<std::uint64_t>(mantissa);
        char* p = end;
        for (int i = 0; i < decimals; ++i) {
            *--p = static_cast<char>('0' + m % 10);
            m /= 10;
        }
        if (decimals > 0) *--p = '.';
        p = detail::put_uint(p, m);
        if (negative) *--p = '-';
        add(tag, std::string_view(p, static_cast<std::size_t>(end - p)));
    }

    // The whole message, or empty on overflow.
    std::string_view finish() noexcept {
        if (overflow_ || !room(trailer_length)) return {};
        const std::size_t body_length = pos_ - prefix_room;
        char digits[max_body_length_digits + 1];
        char* d = detail::put_uint(digits + sizeof(digits), body_length);
        const std::size_t dl = static_cast<std::size_t>(digits + sizeof(digits) - d);
        const std::size_t start = prefix_room - (begin_string.size() + dl + 1);
        std::memcpy(buf_ + start, begin_string.data(), begin_string.size());
        std::memcpy(buf_ + start + begin_string.size(), d, dl);
        buf_[prefix_room - 1] = soh;
        const unsigned sum = checksum(buf_ + start, pos_ - start);
        char* t = buf_ + pos_;
        t[0] = '1';
        t[1] = '0';
        t[2] = '=';
        detail::put_fixed(t + 3, sum, 3);
        t[6] = soh;
        pos_ += trailer_length;
        return {buf_ + start, pos_ - start};
    }
    bool overflow() const noexcept { return overflow_; }
    // Bytes written so far, from the buffer's start (the body begins at prefix_room).
    std::size_t position() const noexcept { return pos_; }

private:
    bool room(std::size_t n) noexcept {
        if (pos_ + n > cap_) overflow_ = true;
        return !overflow_;
    }
    char* buf_;
    std::size_t cap_, pos_ = 0;
    bool overflow_ = false;
};

// The fields of a NewOrderSingle that a template fixes: the same for every
// order it sends.
struct new_order_fixed {
    std::string_view sender, target, account, symbol;
    char side, ord_type, time_in_force;
    int price_decimals;
};

// Representation C: a NewOrderSingle laid out once, with fixed-width slots for
// what changes per order (MsgSeqNum, SendingTime, ClOrdID, TransactTime,
// OrderQty, Price), integers zero-padded as FIX permits. The body length is
// then constant, so the prefix is written once; per order, only the slots and
// the CheckSum are written, and the CheckSum is the constant bytes' sum plus
// the slots' sums. Field order and content otherwise as encoder B writes them.
class new_order_template {
public:
    static constexpr int seq_width = 9, clordid_width = 16, qty_width = 9, price_width = 12;
    static constexpr std::size_t capacity = 512;

    explicit new_order_template(const new_order_fixed& f) noexcept : decimals_(f.price_decimals) {
        encoder e(buf_, capacity);
        e.begin("D");
        e.add(49, f.sender);
        e.add(56, f.target);
        slot(e, 34, seq_width, seq_);
        slot(e, 52, static_cast<int>(utc_timestamp_length), sending_time_);
        slot(e, 11, clordid_width, clordid_);
        e.add(1, f.account);
        e.add(55, f.symbol);
        e.add(54, f.side);
        slot(e, 60, static_cast<int>(utc_timestamp_length), transact_time_);
        slot(e, 38, qty_width, qty_);
        e.add(40, f.ord_type);
        slot(e, 44, price_width, price_);
        e.add(59, f.time_in_force);
        const std::string_view m = e.finish();
        ok_ = !m.empty();
        start_ = static_cast<std::size_t>(m.data() - buf_);
        length_ = m.size();
        // Offsets now from the frame's start; the constant bytes' sum excludes
        // the slots and the trailer.
        for (std::size_t* s : {&seq_, &sending_time_, &clordid_, &transact_time_, &qty_, &price_}) *s -= start_;
        base_sum_ = 0;
        const char* p = buf_ + start_;
        for (std::size_t i = 0; i + trailer_length < length_; ++i) base_sum_ += static_cast<unsigned char>(p[i]);
        base_sum_ -= slot_sum(seq_, seq_width) + slot_sum(sending_time_, utc_timestamp_length) +
                     slot_sum(clordid_, clordid_width) + slot_sum(transact_time_, utc_timestamp_length) +
                     slot_sum(qty_, qty_width) + slot_sum(price_, price_width);
    }

    bool ok() const noexcept { return ok_; }

    // One order: writes the slots and the CheckSum, returns the message.
    // Empty if a value does not fit its slot. `now_ms` is milliseconds since
    // the epoch, UTC; SendingTime and TransactTime both take it.
    std::string_view fill(std::uint64_t seq, std::int64_t now_ms, std::uint64_t clordid, std::uint64_t qty,
                          std::uint64_t price_mantissa) noexcept {
        char* p = buf_ + start_;
        bool fits = detail::put_fixed(p + seq_, seq, seq_width) &&
                    detail::put_fixed(p + clordid_, clordid, clordid_width) &&
                    detail::put_fixed(p + qty_, qty, qty_width) && put_price(p + price_, price_mantissa);
        if (!fits) return {};
        format_utc_timestamp(now_ms, p + sending_time_);
        std::memcpy(p + transact_time_, p + sending_time_, utc_timestamp_length);
        const unsigned sum = base_sum_ + slot_sum(seq_, seq_width) + 2 * slot_sum(sending_time_, utc_timestamp_length) +
                             slot_sum(clordid_, clordid_width) + slot_sum(qty_, qty_width) +
                             slot_sum(price_, price_width);
        detail::put_fixed(p + length_ - trailer_length + 3, sum & 0xffU, 3);
        return {p, length_};
    }

private:
    // Appends `tag=` and `width` placeholder zeros; records where they start.
    void slot(encoder& e, std::uint32_t tag, int width, std::size_t& at) noexcept {
        char zeros[32];
        std::memset(zeros, '0', sizeof(zeros));
        const std::string_view v(zeros, static_cast<std::size_t>(width));
        e.add(tag, v);
        at = e.position() - 1 - static_cast<std::size_t>(width);  // the value ends before its SOH
    }
    unsigned slot_sum(std::size_t at, std::size_t width) const noexcept {
        unsigned s = 0;
        const char* p = buf_ + start_ + at;
        for (std::size_t i = 0; i < width; ++i) s += static_cast<unsigned char>(p[i]);
        return s;
    }
    // The price as zero-padded digits, with a decimal point if it has decimals:
    // 12 characters.
    bool put_price(char* at, std::uint64_t mantissa) const noexcept {
        if (decimals_ == 0) return detail::put_fixed(at, mantissa, price_width);
        const int int_width = price_width - 1 - decimals_;
        if (int_width < 1) return false;
        std::uint64_t scale = 1;
        for (int i = 0; i < decimals_; ++i) scale *= 10;
        if (!detail::put_fixed(at, mantissa / scale, int_width)) return false;
        at[int_width] = '.';
        return detail::put_fixed(at + int_width + 1, mantissa % scale, decimals_);
    }

    char buf_[capacity] = {};
    std::size_t start_ = 0, length_ = 0;
    std::size_t seq_ = 0, sending_time_ = 0, clordid_ = 0, transact_time_ = 0, qty_ = 0, price_ = 0;
    unsigned base_sum_ = 0;
    int decimals_;
    bool ok_ = false;
};

}  // namespace fixgw
