// Checking a decoded message against the FIX 4.4 dictionary (scenarios 2q, 2r
// and 14 of the Session Layer Test Cases): MsgType valid and supported, and a
// session-layer message's fields allowed, present, well formed, in range, not
// repeated, and its repeating groups whole and in order. The tables are
// generated from the FIX Trading Community's orchestrations at build time
// (tools/gen_dictionary.py). Design: docs/design/006-conformance.md
#pragma once

#include <fixgw/decode.hpp>
#include <fixgw/fix44_dictionary.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>

namespace fixgw {

namespace detail {

inline std::optional<std::uint64_t> parse_uint(std::string_view s) noexcept {
    if (s.empty() || s.size() > 19) return std::nullopt;
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's public algorithm).
inline std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146'097 + doe - 719'468;
}

// A UTCTimestamp, "YYYYMMDD-HH:MM:SS" or with ".sss", as milliseconds since 1970.
inline std::optional<std::int64_t> parse_utc_timestamp(std::string_view s) noexcept {
    if (s.size() != 17 && s.size() != 21) return std::nullopt;
    const auto num = [&](std::size_t at, std::size_t n) -> std::int64_t {
        std::int64_t v = 0;
        for (std::size_t i = at; i < at + n; ++i) {
            if (s[i] < '0' || s[i] > '9') return -1;
            v = v * 10 + (s[i] - '0');
        }
        return v;
    };
    if (s[8] != '-' || s[11] != ':' || s[14] != ':' || (s.size() == 21 && s[17] != '.')) return std::nullopt;
    const std::int64_t y = num(0, 4), mo = num(4, 2), d = num(6, 2), h = num(9, 2), mi = num(12, 2), se = num(15, 2);
    const std::int64_t ms = s.size() == 21 ? num(18, 3) : 0;
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60 ||
        ms < 0)
        return std::nullopt;
    return ((days_from_civil(y, mo, d) * 24 + h) * 60 + mi) * 60'000 + se * 1000 + ms;
}

// D2: defined in the specification, or in the user-defined range.
inline constexpr bool tag_defined(std::uint32_t t) noexcept {
    if (t >= 5000 && t <= 9999) return true;
    return t <= fix44::highest_tag && ((fix44::defined_tags[t / 64] >> (t % 64)) & 1) != 0;
}

inline constexpr const fix44::field_info* find_field(std::uint32_t t) noexcept {
    std::size_t lo = 0, hi = std::size(fix44::fields);
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (fix44::fields[mid].tag < t) lo = mid + 1;
        else hi = mid;
    }
    return lo < std::size(fix44::fields) && fix44::fields[lo].tag == t ? &fix44::fields[lo] : nullptr;
}

inline constexpr const fix44::layout* find_layout(std::string_view name) noexcept {
    for (const auto& l : fix44::layouts)
        if (l.name == name) return &l;
    return nullptr;
}

inline constexpr bool digits(std::string_view v) noexcept {
    if (v.empty()) return false;
    for (const char c : v)
        if (c < '0' || c > '9') return false;
    return true;
}

// 0 if the value suits the field, otherwise the SessionRejectReason.
inline int check_value(const fix44::field_info& f, std::string_view v) noexcept {
    switch (f.k) {
        case fix44::kind::string:
        case fix44::kind::data:
            return 0;
        case fix44::kind::length:
        case fix44::kind::seq_num:
        case fix44::kind::num_in_group:
            return digits(v) ? 0 : reject_reason::incorrect_data_format;
        case fix44::kind::integer:
            return digits(!v.empty() && v[0] == '-' ? v.substr(1) : v) ? 0 : reject_reason::incorrect_data_format;
        case fix44::kind::utc_timestamp:
            return parse_utc_timestamp(v) ? 0 : reject_reason::incorrect_data_format;
        case fix44::kind::code_set:
            for (std::size_t i = f.codes_first; i < f.codes_first + f.codes_count; ++i)
                if (fix44::codes[i] == v) return 0;
            return reject_reason::value_incorrect;
    }
    return reject_reason::other;
}

}  // namespace detail

// D4: a FIX 4.4 MsgType, or a user-defined one (beginning with 'U').
inline bool valid_msg_type(std::string_view t) noexcept {
    if (!t.empty() && t[0] == 'U') return true;
    std::size_t lo = 0, hi = std::size(fix44::msg_types);
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (fix44::msg_types[mid] < t) lo = mid + 1;
        else hi = mid;
    }
    return lo < std::size(fix44::msg_types) && fix44::msg_types[lo] == t;
}

// Whether `t` is one of the space-separated MsgTypes in `list`.
inline constexpr bool listed(std::string_view list, std::string_view t) noexcept {
    while (!list.empty()) {
        const std::size_t sp = list.find(' ');
        if (list.substr(0, sp) == t) return true;
        if (sp == std::string_view::npos) break;
        list.remove_prefix(sp + 1);
    }
    return false;
}

// D3: one pass over a session-layer message's fields, in order, against its
// header, body and trailer in the dictionary; the first failure. `type` must
// have a layout (the seven session messages).
template <std::size_t N>
decode_result validate(const message_view<N>& m, std::string_view type) noexcept {
    using namespace fix44;
    const auto fail = [](int reason, std::uint32_t tag, const char* why) noexcept { return decode_result{false, reason, tag, why}; };
    const layout* parts[3] = {detail::find_layout("header"), detail::find_layout(type), detail::find_layout("trailer")};
    if (!parts[0] || !parts[1] || !parts[2]) return fail(reject_reason::other, 0, "no dictionary entry for this MsgType");
    // One bit per member of the three parts, for repeats and required fields.
    std::uint64_t seen = 0;
    const auto bit = [&](std::size_t part, std::size_t k) noexcept {
        std::size_t at = k;
        for (std::size_t p = 0; p < part; ++p) at += parts[p]->count;
        return std::uint64_t{1} << at;
    };
    const group_info* g = nullptr;  // the group being read, if any
    std::uint64_t declared = 0, entries = 0;
    std::size_t last = 0;  // the group member read last in this entry

    for (std::size_t i = 0; i < m.size(); ++i) {
        const std::uint32_t tag = m[i].tag;
        const std::string_view v = m.value(i);
        if (g) {
            std::size_t k = 0;
            while (k < g->count && group_members[g->first + k].tag != tag) ++k;
            if (k < g->count) {
                if (k == 0) {
                    if (++entries > declared) return fail(reject_reason::incorrect_num_in_group, g->count_tag, "more group entries than NumInGroup");
                } else if (entries == 0 || k <= last) {
                    return fail(reject_reason::group_fields_out_of_order, tag, "repeating group fields out of order");
                }
                last = k;
                const field_info* f = detail::find_field(tag);
                if (const int r = f ? detail::check_value(*f, v) : 0) return fail(r, tag, r == reject_reason::value_incorrect ? "value is incorrect (out of range) for this tag" : "incorrect data format for value");
                continue;
            }
            if (entries != declared) return fail(reject_reason::incorrect_num_in_group, g->count_tag, "fewer group entries than NumInGroup");
            g = nullptr;
        }
        std::size_t part = 0, k = 0;
        for (; part < 3; ++part) {
            for (k = 0; k < parts[part]->count && members[parts[part]->first + k].tag != tag; ++k) {
            }
            if (k < parts[part]->count) break;
        }
        if (part == 3)
            return detail::tag_defined(tag) ? fail(reject_reason::tag_not_defined_for_message_type, tag, "tag not defined for this message type")
                                            : fail(reject_reason::invalid_tag_number, tag, "invalid tag number");
        const std::uint64_t b = bit(part, k);
        if (seen & b) return fail(reject_reason::tag_repeated, tag, "tag appears more than once");
        seen |= b;
        const field_info* f = detail::find_field(tag);
        if (const int r = f ? detail::check_value(*f, v) : 0)
            return fail(r, tag, r == reject_reason::value_incorrect ? "value is incorrect (out of range) for this tag" : "incorrect data format for value");
        const member& mem = members[parts[part]->first + k];
        if (mem.group) {
            g = &groups[mem.group - 1];
            declared = detail::parse_uint(v).value_or(~std::uint64_t{0});  // digits, checked above; too long never matches
            entries = 0;
            last = 0;
        }
    }
    if (g && entries != declared) return fail(reject_reason::incorrect_num_in_group, g->count_tag, "fewer group entries than NumInGroup");
    for (std::size_t part = 0; part < 3; ++part)
        for (std::size_t k = 0; k < parts[part]->count; ++k) {
            const member& mem = members[parts[part]->first + k];
            if (mem.required && !(seen & bit(part, k))) return fail(reject_reason::required_tag_missing, mem.tag, "required tag missing");
        }
    return {true, 0, 0, nullptr};
}

}  // namespace fixgw
