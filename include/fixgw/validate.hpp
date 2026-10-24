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

// v[at, at + n), the caller having checked the bounds: substr's range check
// throws, which the hot path must not (note 003, S6).
inline constexpr std::string_view slice(std::string_view v, std::size_t at, std::size_t n) noexcept {
    return {v.data() + at, n};
}

inline constexpr bool digits(std::string_view v) noexcept {
    if (v.empty()) return false;
    for (const char c : v)
        if (c < '0' || c > '9') return false;
    return true;
}

// FIX's float: an optional '-', digits with at most one '.', a digit somewhere.
inline constexpr bool decimal(std::string_view v) noexcept {
    if (!v.empty() && v[0] == '-') v.remove_prefix(1);
    bool digit = false, point = false;
    for (const char c : v) {
        if (c >= '0' && c <= '9') digit = true;
        else if (c == '.' && !point) point = true;
        else return false;
    }
    return digit;
}

// YYYYMMDD.
inline constexpr bool date(std::string_view v) noexcept {
    if (v.size() != 8 || !digits(v)) return false;
    const int m = (v[4] - '0') * 10 + (v[5] - '0'), d = (v[6] - '0') * 10 + (v[7] - '0');
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

// HH:MM:SS or HH:MM:SS.sss.
inline constexpr bool time_only(std::string_view v) noexcept {
    if ((v.size() != 8 && v.size() != 12) || v[2] != ':' || v[5] != ':' || (v.size() == 12 && v[8] != '.')) return false;
    const auto two = [&](std::size_t i) { return digits(slice(v, i, 2)) ? (v[i] - '0') * 10 + (v[i + 1] - '0') : -1; };
    return two(0) >= 0 && two(0) <= 23 && two(3) >= 0 && two(3) <= 59 && two(6) >= 0 && two(6) <= 60 &&
           (v.size() == 8 || digits(slice(v, 9, 3)));
}

// YYYYMM, YYYYMMDD, or YYYYMMwN (week N).
inline constexpr bool month_year(std::string_view v) noexcept {
    if (v.size() < 6 || !digits(slice(v, 0, 6))) return false;
    const int m = (v[4] - '0') * 10 + (v[5] - '0');
    if (m < 1 || m > 12) return false;
    if (v.size() == 6) return true;
    if (v.size() == 8 && v[6] == 'w') return v[7] >= '1' && v[7] <= '5';
    return v.size() == 8 && date(v);
}

inline bool in_codes(const fix44::field_info& f, std::string_view v) noexcept {
    for (std::size_t i = f.codes_first; i < f.codes_first + f.codes_count; ++i)
        if (fix44::codes[i] == v) return true;
    return false;
}

// 0 if the value suits the field, otherwise the SessionRejectReason.
inline int check_value(const fix44::field_info& f, std::string_view v) noexcept {
    using fix44::kind;
    switch (f.k) {
        case kind::string:
        case kind::data:
            return 0;
        case kind::length:
        case kind::seq_num:
        case kind::num_in_group:
            return digits(v) ? 0 : reject_reason::incorrect_data_format;
        case kind::integer:
            return digits(!v.empty() && v[0] == '-' ? slice(v, 1, v.size() - 1) : v) ? 0 : reject_reason::incorrect_data_format;
        case kind::decimal:
            return decimal(v) ? 0 : reject_reason::incorrect_data_format;
        case kind::single_char:
            return v.size() == 1 ? 0 : reject_reason::incorrect_data_format;
        case kind::boolean:
            return v == "Y" || v == "N" ? 0 : reject_reason::incorrect_data_format;
        case kind::utc_timestamp:
            return parse_utc_timestamp(v) ? 0 : reject_reason::incorrect_data_format;
        case kind::utc_time_only:
            return time_only(v) ? 0 : reject_reason::incorrect_data_format;
        case kind::date:
            return date(v) ? 0 : reject_reason::incorrect_data_format;
        case kind::month_year:
            return month_year(v) ? 0 : reject_reason::incorrect_data_format;
        case kind::code_set:
            if (in_codes(f, v)) return 0;
            if (f.union_from && digits(v) && v.size() < 10 && parse_uint(v).value_or(0) >= f.union_from) return 0;  // user-defined
            if (f.union_qty && decimal(v)) return 0;
            return reject_reason::value_incorrect;
        case kind::multi_code_set:  // space-separated, each one of the codes
            while (!v.empty()) {
                const std::size_t sp = v.find(' ');
                if (!in_codes(f, slice(v, 0, sp == std::string_view::npos ? v.size() : sp))) return reject_reason::value_incorrect;
                if (sp == std::string_view::npos) break;
                v.remove_prefix(sp + 1);
            }
            return 0;
    }
    return reject_reason::other;
}

inline constexpr const fix44::message_def* find_message(std::string_view type) noexcept {
    for (const auto& m : fix44::messages)
        if (m.type == type) return &m;
    return nullptr;
}

// The node for `tag` in a message's run, or nullptr.
inline constexpr const fix44::node* find_node(const fix44::message_def& m, std::uint32_t tag) noexcept {
    std::size_t lo = m.first, hi = m.first + m.count;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (fix44::nodes[mid].tag < tag) lo = mid + 1;
        else hi = mid;
    }
    return lo < m.first + m.count && fix44::nodes[lo].tag == tag ? &fix44::nodes[lo] : nullptr;
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

// Whether the dictionary has an entry for `type` (the session messages and
// the order-entry ones).
inline bool has_dictionary(std::string_view type) noexcept { return detail::find_message(type) != nullptr; }

// One pass over a message's fields, in order, against its dictionary entry
// (note 006 D3, note 007 D8); the first failure. Repeating groups may nest:
// each open group is checked for its entries' count (16) and its fields'
// order within an entry (15). Required fields are checked at the top level;
// within an entry only its first field is (it starts the entry).
namespace detail {

// The fields of a decoded message, in order.
template <std::size_t N>
struct view_fields {
    const message_view<N>& m;
    std::size_t i = 0;
    bool next(std::uint32_t& tag, std::string_view& v) noexcept {
        if (i == m.size()) return false;
        tag = m[i].tag;
        v = m.value(i++);
        return true;
    }
    std::optional<std::string_view> get(std::uint32_t tag) const noexcept { return m.get(tag); }
};

// The fields of a body as the application submits it ("tag=value" SOH ...).
struct body_fields {
    std::string_view b;
    std::size_t i = 0;
    bool malformed = false;
    bool next(std::uint32_t& tag, std::string_view& v) noexcept {
        if (i >= b.size()) return false;
        std::uint32_t t = 0;
        std::size_t j = i;
        while (j < b.size() && b[j] >= '0' && b[j] <= '9' && t < 100'000'000) t = t * 10 + static_cast<std::uint32_t>(b[j++] - '0');
        const std::size_t end = j < b.size() ? b.find(soh, j) : std::string_view::npos;
        if (j == i || j >= b.size() || b[j] != '=' || end == std::string_view::npos || end == j + 1) {
            malformed = true;
            return false;
        }
        tag = t;
        v = slice(b, j + 1, end - j - 1);
        i = end + 1;
        return true;
    }
    std::optional<std::string_view> get(std::uint32_t tag) const noexcept {
        body_fields c{b};
        std::uint32_t t;
        std::string_view v;
        while (c.next(t, v))
            if (t == tag) return v;
        return std::nullopt;
    }
};

// Header and trailer fields a body does not carry (the session adds them).
inline constexpr bool session_added(std::uint32_t t) noexcept {
    return t == 8 || t == 9 || t == 35 || t == 49 || t == 56 || t == 34 || t == 52 || t == 10;
}

template <class Fields>
decode_result validate_fields(Fields&& fields, std::string_view type, bool body_only) noexcept;

}  // namespace detail

template <std::size_t N>
decode_result validate(const message_view<N>& m, std::string_view type) noexcept {
    return detail::validate_fields(detail::view_fields<N>{m}, type, false);
}

// An application body about to be sent (note 007): the same checks, but the
// fields the session adds (header and trailer) are not looked for.
inline decode_result validate_body(std::string_view type, std::string_view body) noexcept {
    detail::body_fields f{body};
    const decode_result r = detail::validate_fields(f, type, true);
    if (r.ok && f.malformed) return {false, reject_reason::other, 0, "body is not tag=value fields"};
    return r;
}

template <class Fields>
decode_result detail::validate_fields(Fields&& m, std::string_view type, bool body_only) noexcept {
    using namespace fix44;
    const auto fail = [](int reason, std::uint32_t tag, const char* why) noexcept { return decode_result{false, reason, tag, why}; };
    const message_def* def = detail::find_message(type);
    if (!def) return fail(reject_reason::other, 0, "no dictionary entry for this MsgType");
    constexpr std::size_t max_nodes = 512, max_depth = 4;
    if (def->count > max_nodes) return fail(reject_reason::other, 0, "dictionary entry too large");
    std::uint64_t seen[max_nodes / 64] = {};
    struct open_group {
        std::size_t node;  // the NumInGroup's index in nodes
        std::uint64_t declared, entries;
        unsigned last;  // position of the field read last in this entry
    } open[max_depth];
    std::size_t depth = 0;
    const auto close_one = [&]() noexcept { --depth; return open[depth].entries == open[depth].declared; };

    std::uint32_t tag;
    std::string_view v;
    while (m.next(tag, v)) {
        const node* n = detail::find_node(*def, tag);
        if (!n)
            return detail::tag_defined(tag) ? fail(reject_reason::tag_not_defined_for_message_type, tag, "tag not defined for this message type")
                                            : fail(reject_reason::invalid_tag_number, tag, "invalid tag number");
        // Close the groups this field is not in.
        while (depth > 0 && n->parent != open[depth - 1].node) {
            bool inside = false;  // in a group nested in the open one, not yet opened: out of order
            for (std::size_t p = n->parent; p != no_parent; p = nodes[p].parent)
                if (p == open[depth - 1].node) inside = true;
            if (inside) return fail(reject_reason::group_fields_out_of_order, tag, "repeating group fields out of order");
            const std::uint32_t count_tag = nodes[open[depth - 1].node].tag;
            if (!close_one()) return fail(reject_reason::incorrect_num_in_group, count_tag, "group entries not as NumInGroup says");
        }
        if (n->parent != no_parent) {
            if (depth == 0 || open[depth - 1].node != n->parent)
                return fail(reject_reason::group_fields_out_of_order, tag, "repeating group fields out of order");
            open_group& g = open[depth - 1];
            if (n->position == 0) {
                if (++g.entries > g.declared) return fail(reject_reason::incorrect_num_in_group, nodes[g.node].tag, "group entries not as NumInGroup says");
            } else if (g.entries == 0 || n->position <= g.last) {
                return fail(reject_reason::group_fields_out_of_order, tag, "repeating group fields out of order");
            }
            g.last = n->position;
        } else {
            const std::size_t k = static_cast<std::size_t>(n - &nodes[def->first]);
            if (seen[k / 64] & (std::uint64_t{1} << (k % 64))) return fail(reject_reason::tag_repeated, tag, "tag appears more than once");
            seen[k / 64] |= std::uint64_t{1} << (k % 64);
        }
        const field_info* f = detail::find_field(tag);
        if (const int r = f ? detail::check_value(*f, v) : 0)
            return fail(r, tag, r == reject_reason::value_incorrect ? "value is incorrect (out of range) for this tag" : "incorrect data format for value");
        if (n->group) {
            if (depth == max_depth) return fail(reject_reason::other, tag, "repeating groups nested too deep");
            open[depth++] = {static_cast<std::size_t>(n - nodes), detail::parse_uint(v).value_or(~std::uint64_t{0}), 0, 0};
        }
    }
    while (depth > 0) {
        const std::uint32_t count_tag = nodes[open[depth - 1].node].tag;
        if (!close_one()) return fail(reject_reason::incorrect_num_in_group, count_tag, "group entries not as NumInGroup says");
    }
    for (std::size_t k = 0; k < def->count; ++k) {
        const node& n = nodes[def->first + k];
        if (n.parent == no_parent && n.required && !(seen[k / 64] & (std::uint64_t{1} << (k % 64))) &&
            !(body_only && detail::session_added(n.tag)))
            return fail(reject_reason::required_tag_missing, n.tag, "required tag missing");
    }
    return {true, 0, 0, nullptr};
}

// Volume 4's conditional requirements for the fields the order state machine
// uses (note 007, D8): stated in its prose, not in the orchestration, so
// written here. A failure is answered with BusinessMessageReject, reason 5
// (scenario 14m); RefTagID names the missing field.
namespace business_reason {
inline constexpr int unsupported_message_type = 3;
inline constexpr int conditionally_required_field_missing = 5;
}  // namespace business_reason

template <class Get>
decode_result conditional_rules_of(const Get& m, std::string_view type) noexcept {
    const auto missing = [&](std::uint32_t tag) noexcept { return !m.get(tag).has_value(); };
    const auto fail = [](std::uint32_t tag) noexcept {
        return decode_result{false, business_reason::conditionally_required_field_missing, tag, "Conditionally required field missing"};
    };
    if (type == "D" || type == "G") {
        const std::string_view ord_type = m.get(40).value_or("");
        // Price for the limit types: Limit, Stop limit, Limit or better, Limit with or without.
        if ((ord_type == "2" || ord_type == "4" || ord_type == "7" || ord_type == "8") && missing(44)) return fail(44);
        if ((ord_type == "3" || ord_type == "4") && missing(99)) return fail(99);  // StopPx for stops
        if (m.get(59) == std::string_view("6") && missing(432) && missing(126)) return fail(432);  // GTD: an expiry
        if (missing(38) && missing(152) && missing(516)) return fail(38);  // one of the OrderQtyData quantities
    } else if (type == "8") {
        const std::string_view exec_type = m.get(150).value_or("");
        // OrigClOrdID when answering a cancel or replace (PendingCancel, Replaced);
        // Canceled may be unsolicited (E.1.b), so the state machine judges it.
        if ((exec_type == "6" || exec_type == "5") && missing(41)) return fail(41);
        if ((exec_type == "H" || exec_type == "G") && missing(19)) return fail(19);  // ExecRefID for a bust or correction
        if (exec_type == "D" && missing(378)) return fail(378);  // ExecRestatementReason
        if (exec_type == "F" || exec_type == "G") {  // LastQty, LastPx for a fill or its correction
            if (missing(32)) return fail(32);
            if (missing(31)) return fail(31);
        }
    }
    return {true, 0, 0, nullptr};
}

template <std::size_t N>
decode_result conditional_rules(const message_view<N>& m, std::string_view type) noexcept {
    return conditional_rules_of(m, type);
}
inline decode_result conditional_rules_body(std::string_view type, std::string_view body) noexcept {
    return conditional_rules_of(detail::body_fields{body}, type);
}

}  // namespace fixgw
