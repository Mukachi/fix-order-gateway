// The order state machine (note 007, D1): one entry per order, followed through
// its chain of ClOrdIDs (each cancel or replace adds one), built from what the
// application sends and from the ExecutionReports and OrderCancelRejects the
// venue sends back. Also the duplicate-report check (D3), the outbound
// throttle (D4) and the stale-order rule (D5). Sans-I/O and deterministic, so
// journal replay rebuilds it; fixed capacity, so nothing allocates after
// construction. Design: docs/design/007-order-entry.md
#pragma once

#include <fixgw/decode.hpp>
#include <fixgw/framing.hpp>
#include <fixgw/session.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <optional>
#include <string_view>
#include <vector>

namespace fixgw {

struct order_config {
    std::size_t max_orders = 65'536;
    std::size_t max_exec_ids = 1'000'000;      // D3: for the session's life
    std::size_t id_bytes = 32 * 1'000'000;      // room for every ClOrdID and ExecID kept
    std::size_t throttle_messages = 0;          // D4: N per window; 0 is no throttle
    std::int64_t throttle_window_ns = 1'000'000'000;
    std::int64_t stale_ns = 2'000'000'000;      // D5: an order or replace older than this is not resent
};

// Quantities and prices as integers in units of 1e-8 (FIX's floats carried
// exactly enough for share quantities and prices).
using fixed = std::int64_t;
inline constexpr fixed fixed_one = 100'000'000;

namespace detail {

// A FIX float as fixed; nullopt if it is not one or has more than 8 decimals.
inline std::optional<fixed> parse_fixed(std::string_view v) noexcept {
    bool negative = false;
    if (!v.empty() && v[0] == '-') {
        negative = true;
        v.remove_prefix(1);
    }
    if (v.empty()) return std::nullopt;
    std::int64_t whole = 0, frac = 0;
    int frac_digits = -1;
    for (const char c : v) {
        if (c == '.') {
            if (frac_digits >= 0) return std::nullopt;
            frac_digits = 0;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        if (frac_digits < 0) {
            if (whole > 92'233'720'368) return std::nullopt;  // would overflow when scaled
            whole = whole * 10 + (c - '0');
        } else {
            if (++frac_digits > 8) return std::nullopt;
            frac = frac * 10 + (c - '0');
        }
    }
    for (int d = frac_digits < 0 ? 0 : frac_digits; d < 8; ++d) frac *= 10;
    const fixed r = whole * fixed_one + frac;
    return negative ? -r : r;
}

// A field of a body as the application submits it ("tag=value" SOH ...).
inline std::optional<std::string_view> body_field(std::string_view body, std::uint32_t tag) noexcept {
    std::size_t i = 0;
    while (i < body.size()) {
        std::uint32_t t = 0;
        std::size_t j = i;
        while (j < body.size() && body[j] >= '0' && body[j] <= '9') t = t * 10 + static_cast<std::uint32_t>(body[j++] - '0');
        if (j >= body.size() || body[j] != '=') return std::nullopt;
        const std::size_t end = body.find(soh, j + 1);
        const std::size_t stop = end == std::string_view::npos ? body.size() : end;
        if (t == tag) return std::string_view(body.data() + j + 1, stop - j - 1);
        i = stop + 1;
    }
    return std::nullopt;
}

inline constexpr std::uint64_t fnv1a(std::string_view s) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : s) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return h;
}

}  // namespace detail

// OrdStatus (39) values the machine uses.
namespace ord_status {
inline constexpr char new_ = '0', partially_filled = '1', filled = '2', done_for_day = '3', canceled = '4',
                      pending_cancel = '6', rejected = '8', pending_new = 'A', calculated = 'B', expired = 'C',
                      pending_replace = 'E';
inline constexpr bool finished(char s) noexcept {
    return s == filled || s == canceled || s == rejected || s == expired || s == calculated;
}
// Finished and cannot be reopened (a filled order can, by a replace raising its quantity).
inline constexpr bool dead(char s) noexcept { return s == canceled || s == rejected || s == expired; }
}  // namespace ord_status

// What the application learns about an order besides the venue's messages.
struct order_event {
    enum : std::uint8_t { state = 1, not_sent = 2, refused = 3 } kind;
    char ord_status;             // state: the order's OrdStatus after the report
    char exec_type;              // state: the report's ExecType ('9' for an OrderCancelReject)
    std::uint8_t id_length;
    char cl_ord_id[40];          // the order's current ClOrdID (refused: the request's)
    fixed order_qty, cum_qty, leaves_qty, avg_px;
    std::int64_t retry_at_ns;    // refused by the throttle: when a retry may pass
    bool inconsistent;           // state: the report broke an invariant (D1); logged too
    std::uint8_t reason_length;
    char reason[64];
    std::string_view id() const noexcept { return {cl_ord_id, id_length}; }
    std::string_view why() const noexcept { return {reason, reason_length}; }
};

class order_book {
public:
    // The tables come zeroed from calloc, so the pages are the kernel's zero
    // page until written: a session pays for what it uses, not for the
    // capacity (~100 MB at the defaults). All-zero is an empty entry.
    explicit order_book(const order_config& c = {})
        : cfg_(c), orders_(zeroed<order>(c.max_orders)), orders_size_(c.max_orders),
          ids_(zeroed<id_entry>(table_size(c.max_orders * 4))), ids_size_(table_size(c.max_orders * 4)),
          exec_ids_(zeroed<id_entry>(table_size(c.max_exec_ids))), exec_ids_size_(table_size(c.max_exec_ids)),
          sends_(c.throttle_messages ? c.throttle_messages : 1) {
        arena_.reserve(c.id_bytes);
    }

    struct verdict {
        bool ok;
        std::int64_t retry_at_ns;  // throttled: when it may pass
        const char* why;
    };

    // An application message about to be sent (D, F or G; others pass):
    // whether it may go. Changes nothing; `record` follows once the session
    // has taken it.
    verdict check(std::string_view type, std::string_view body, session_time now) const {
        if (type != "D" && type != "F" && type != "G") return {true, 0, nullptr};
        const auto id = detail::body_field(body, 11);
        if (!id || id->empty() || id->size() > sizeof(order_event::cl_ord_id)) return {false, 0, "ClOrdID missing or longer than 40"};
        if (find_id(*id) != none) return {false, 0, "duplicate ClOrdID"};
        if (type != "D") {
            const auto orig = detail::body_field(body, 41);
            const std::uint32_t o = orig ? find_id(*orig) : none;
            if (o == none) return {false, 0, "unknown OrigClOrdID"};
            // A cancel needs a live order; a replace may also raise a filled
            // order's quantity (matrices C.1.c, J.1.a), so only a dead one refuses it.
            const char st = orders_[o].status;
            if (type == "F" ? ord_status::finished(st) : ord_status::dead(st)) return {false, 0, "order already finished"};
        } else if (count_ == orders_size_) {
            return {false, 0, "order capacity reached"};
        }
        if ((id_count_ + 1) * 2 > ids_size_ || arena_.size() + id->size() > cfg_.id_bytes) return {false, 0, "ClOrdID capacity reached"};
        // D4: a cancel is counted but never refused (it can only be of a live order here)
        if (type != "F" && throttled(now)) return {false, oldest_send() + cfg_.throttle_window_ns, "throttled"};
        return {true, 0, nullptr};
    }

    // Records a message `check` passed and the session took.
    void record(std::string_view type, std::string_view body, session_time now) {
        if (type != "D" && type != "F" && type != "G") return;
        const std::string_view id = *detail::body_field(body, 11);
        std::uint32_t o;
        if (type == "D") {
            o = static_cast<std::uint32_t>(count_++);
            order& n = orders_[o];
            n = {};
            n.status = ord_status::pending_new;
            n.order_qty = detail::parse_fixed(detail::body_field(body, 38).value_or("")).value_or(0);
            n.leaves_qty = n.order_qty;
            n.submitted_ns = now.mono_ns;
        } else {
            o = find_id(*detail::body_field(body, 41));
            order& n = orders_[o];
            (type == "F" ? n.cancel_pending : n.replace_pending) = true;
            n.request_ns = now.mono_ns;
        }
        add_id(id, o);
        if (type == "D") orders_[o].current = last_id_;
        record_send(now);
    }

    // An ExecutionReport or OrderCancelReject from the venue. False: a
    // duplicate (its ExecID already seen, D3), not to be delivered. `out`
    // gets the order's state after it.
    bool inbound(const message_view<>& m, order_event& out) {
        const std::string_view type = m.msg_type();
        out = {};
        out.kind = order_event::state;
        if (type == "8") {
            const auto exec_id = m.get(17).value_or("");
            if (!exec_id.empty()) {
                if (seen_exec_id(exec_id)) return false;
                if (!add_exec_id(exec_id)) out.inconsistent = true;  // full: logged by the caller, never a silent miss
            }
        } else if (type != "9") {
            return true;
        }
        const std::string_view id = m.get(11).value_or(""), orig = m.get(41).value_or("");
        std::uint32_t o = find_id(id);
        if (o == none && !orig.empty()) o = find_id(orig);
        fill_id(out, id);
        out.exec_type = type == "9" ? '9' : m.get(150).value_or("?")[0];
        out.ord_status = m.get(39).value_or("?")[0];
        if (o == none) {  // not one of ours (E.1.a, telephoned orders: out of scope)
            set_reason(out, "unknown ClOrdID");
            out.inconsistent = true;
            return true;
        }
        order& n = orders_[o];
        if (type == "9") {
            n.cancel_pending = n.replace_pending = false;  // the request failed; the order stands
            // CxlRejReason 1 (unknown order): FIX 4.4 has OrdStatus say Rejected,
            // of the request's OrigClOrdID, not of our order, which keeps its own.
            if (m.get(102) != std::string_view("1")) n.status = out.ord_status;
        } else {
            apply(n, m, out);
            if (out.exec_type == '5' && find_id(id) != none) n.current = id_index(id);  // replaced: the new ClOrdID
        }
        snapshot(n, out);
        return true;
    }

    // D5, the session's resend hook: whether a stored message we sent may be
    // resent. An order or replace older than stale_ns is not (it is
    // gap-filled, so it never reaches the venue): `not_sent` gets the event.
    bool may_resend(std::string_view type, std::string_view cl_ord_id, session_time now, order_event& not_sent) {
        const std::uint32_t o = find_id(cl_ord_id);
        if (o == none) return type != "D" && type != "G";  // not ours to judge
        order& n = orders_[o];
        if (type == "F") return !ord_status::finished(n.status);
        if (type != "D" && type != "G") return true;
        const std::int64_t at = type == "D" ? n.submitted_ns : n.request_ns;
        if (now.mono_ns - at <= cfg_.stale_ns) return true;
        not_sent = {};
        not_sent.kind = order_event::not_sent;
        fill_id(not_sent, cl_ord_id);
        if (type == "D") n.status = ord_status::rejected;
        else n.replace_pending = false;
        set_reason(not_sent, "stale: not sent");
        snapshot(n, not_sent);
        return false;
    }

    std::size_t orders() const noexcept { return count_; }
    std::size_t exec_ids() const noexcept { return exec_count_; }

    // The state of the order whose chain includes `cl_ord_id`, if any.
    std::optional<order_event> state(std::string_view cl_ord_id) const {
        const std::uint32_t o = find_id(cl_ord_id);
        if (o == none) return std::nullopt;
        order_event e{};
        e.kind = order_event::state;
        fill_id(e, current_id(orders_[o]));
        snapshot(orders_[o], e);
        return e;
    }

private:
    static constexpr std::uint32_t none = 0xffffffffu;
    struct order {
        char status = ord_status::pending_new;
        bool cancel_pending = false, replace_pending = false;
        fixed order_qty = 0, cum_qty = 0, leaves_qty = 0, avg_px = 0;
        std::int64_t submitted_ns = 0, request_ns = 0;
        std::uint32_t current = none;  // index in ids_ of the current ClOrdID
    };
    struct id_entry {
        std::uint64_t hash = 0;
        std::uint32_t offset = 0;
        std::uint16_t length = 0;
        bool used = false;
        std::uint32_t order = none;
    };

    struct free_deleter {
        void operator()(void* p) const noexcept { std::free(p); }
    };
    template <class T>
    using table = std::unique_ptr<T[], free_deleter>;
    template <class T>
    static table<T> zeroed(std::size_t n) {
        static_assert(std::is_trivially_destructible_v<T> && std::is_trivially_copyable_v<T>);
        void* p = std::calloc(n, sizeof(T));
        if (!p) throw std::bad_alloc();
        return table<T>(static_cast<T*>(p));
    }

    static std::size_t table_size(std::size_t n) {
        std::size_t s = 16;
        while (s < n * 2) s <<= 1;  // at most half full
        return s;
    }

    std::string_view key(const id_entry& e) const noexcept { return {arena_.data() + e.offset, e.length}; }
    std::string_view current_id(const order& n) const noexcept { return n.current == none ? std::string_view{} : key(ids_[n.current]); }
    std::uint32_t id_index(std::string_view id) const noexcept {
        const std::uint64_t h = detail::fnv1a(id);
        for (std::size_t i = h & (ids_size_ - 1);; i = (i + 1) & (ids_size_ - 1)) {
            if (!ids_[i].used) return none;
            if (ids_[i].hash == h && key(ids_[i]) == id) return static_cast<std::uint32_t>(i);
        }
    }
    std::uint32_t find_id(std::string_view id) const noexcept {
        const std::uint32_t i = id.empty() ? none : id_index(id);
        return i == none ? none : ids_[i].order;
    }
    bool store(std::string_view s, std::uint32_t& offset) {
        if (arena_.size() + s.size() > cfg_.id_bytes) return false;
        offset = static_cast<std::uint32_t>(arena_.size());
        arena_.insert(arena_.end(), s.begin(), s.end());  // within the reserved capacity: no allocation
        return true;
    }
    bool add_id(std::string_view id, std::uint32_t o) {
        if ((id_count_ + 1) * 2 > ids_size_) return false;
        const std::uint64_t h = detail::fnv1a(id);
        std::size_t i = h & (ids_size_ - 1);
        while (ids_[i].used) i = (i + 1) & (ids_size_ - 1);
        std::uint32_t off;
        if (!store(id, off)) return false;
        ids_[i] = {h, off, static_cast<std::uint16_t>(id.size()), true, o};
        last_id_ = static_cast<std::uint32_t>(i);
        ++id_count_;
        return true;
    }
    bool seen_exec_id(std::string_view id) const noexcept {
        const std::uint64_t h = detail::fnv1a(id);
        for (std::size_t i = h & (exec_ids_size_ - 1);; i = (i + 1) & (exec_ids_size_ - 1)) {
            if (!exec_ids_[i].used) return false;
            if (exec_ids_[i].hash == h && key(exec_ids_[i]) == id) return true;
        }
    }
    bool add_exec_id(std::string_view id) {
        if ((exec_count_ + 1) * 2 > exec_ids_size_) return false;
        const std::uint64_t h = detail::fnv1a(id);
        std::size_t i = h & (exec_ids_size_ - 1);
        while (exec_ids_[i].used) i = (i + 1) & (exec_ids_size_ - 1);
        std::uint32_t off;
        if (!store(id, off)) return false;
        exec_ids_[i] = {h, off, static_cast<std::uint16_t>(id.size()), true, none};
        ++exec_count_;
        return true;
    }

    // D4: a ring of the last N send times.
    bool throttled(session_time now) const noexcept {
        if (cfg_.throttle_messages == 0 || sends_count_ < cfg_.throttle_messages) return false;
        return now.mono_ns - oldest_send() < cfg_.throttle_window_ns;
    }
    std::int64_t oldest_send() const noexcept { return sends_[sends_head_]; }
    void record_send(session_time now) noexcept {
        if (cfg_.throttle_messages == 0) return;
        sends_[sends_head_] = now.mono_ns;
        sends_head_ = (sends_head_ + 1) % sends_.size();
        if (sends_count_ < sends_.size()) ++sends_count_;
    }

    // An ExecutionReport applied (D1): the venue's OrdStatus and quantities
    // taken, our pending flags cleared by the reports that answer them, and
    // the invariants checked.
    void apply(order& n, const message_view<>& m, order_event& out) {
        const char exec_type = out.exec_type;
        const auto cum = detail::parse_fixed(m.get(14).value_or("")), leaves = detail::parse_fixed(m.get(151).value_or(""));
        const auto avg = detail::parse_fixed(m.get(6).value_or("")), qty = detail::parse_fixed(m.get(38).value_or(""));
        const bool correction = exec_type == 'H' || exec_type == 'G';  // a bust or correction may lower CumQty
        if (cum && *cum < n.cum_qty && !correction) {
            out.inconsistent = true;
            set_reason(out, "CumQty fell");
        }
        n.status = out.ord_status;
        if (cum) n.cum_qty = *cum;
        if (leaves) n.leaves_qty = *leaves;
        if (avg) n.avg_px = *avg;
        if (qty) n.order_qty = *qty;
        if (exec_type == '4' || exec_type == '6') n.cancel_pending = exec_type == '6';
        if (exec_type == '5' || exec_type == 'E') n.replace_pending = exec_type == 'E';
        if (ord_status::finished(n.status)) n.cancel_pending = n.replace_pending = false;
        const bool active = !ord_status::finished(n.status) && n.status != ord_status::done_for_day;
        if (active && qty && cum && leaves && *qty != *cum + *leaves) {
            out.inconsistent = true;
            set_reason(out, "OrderQty is not CumQty plus LeavesQty");
        }
    }
    void snapshot(const order& n, order_event& e) const noexcept {
        e.ord_status = n.status;
        e.order_qty = n.order_qty;
        e.cum_qty = n.cum_qty;
        e.leaves_qty = n.leaves_qty;
        e.avg_px = n.avg_px;
    }
    static void fill_id(order_event& e, std::string_view id) noexcept {
        const std::size_t n = id.size() < sizeof(e.cl_ord_id) ? id.size() : sizeof(e.cl_ord_id);
        std::memcpy(e.cl_ord_id, id.data(), n);
        e.id_length = static_cast<std::uint8_t>(n);
    }
    static void set_reason(order_event& e, std::string_view why) noexcept {
        const std::size_t n = why.size() < sizeof(e.reason) ? why.size() : sizeof(e.reason);
        std::memcpy(e.reason, why.data(), n);
        e.reason_length = static_cast<std::uint8_t>(n);
    }

    order_config cfg_;
    table<order> orders_;
    std::size_t orders_size_;
    std::size_t count_ = 0;
    table<id_entry> ids_;
    std::size_t ids_size_;
    table<id_entry> exec_ids_;
    std::size_t exec_ids_size_;
    std::size_t id_count_ = 0, exec_count_ = 0;
    std::uint32_t last_id_ = none;
    std::vector<char> arena_;
    std::vector<std::int64_t> sends_;
    std::size_t sends_head_ = 0, sends_count_ = 0;
};

}  // namespace fixgw
