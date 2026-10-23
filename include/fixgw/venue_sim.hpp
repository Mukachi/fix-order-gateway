// A venue simulator (note 007, D7): the sell side of the order-entry messages,
// for tests and for note 008 to measure against. One price-time book per
// symbol; limit and market orders, IOC and FOK, part fills, cancel, replace
// (priority kept only for a quantity decrease at the same price), end of day,
// duplicate ClOrdIDs (matrices F.1.a, F.1.b) and cancel-on-disconnect.
// Sans-I/O: messages in by session number, messages out through `emit(session,
// MsgType, body)`. Not on the gateway's hot path: it uses ordinary containers.
// Design: docs/design/007-order-entry.md
#pragma once

#include <fixgw/decode.hpp>
#include <fixgw/orders.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fixgw {

__extension__ using sim_wide = __int128;  // a fill's value, price x quantity, before scaling back

struct venue_config {
    bool pending_acks = true;          // PendingCancel / PendingReplace before the result
    bool cancel_on_disconnect = true;  // a session's live orders canceled when it disconnects
};

class venue_sim {
public:
    explicit venue_sim(const venue_config& c = {}) : cfg_(c) {}

    // Resting liquidity from no session (its fills are not reported).
    void seed(std::string_view symbol, char side, fixed price, fixed qty) {
        order o;
        o.session = -1;
        o.symbol = std::string(symbol);
        o.side = side;
        o.ord_type = '2';
        o.tif = '0';
        o.price = price;
        o.order_qty = o.leaves = qty;
        o.status = ord_status::new_;
        o.order_id = next_order_id();
        o.priority = ++clock_;
        o.live = true;
        orders_.push_back(o);
        rest(orders_.size() - 1);
    }

    template <class Emit>
    void on_message(int session, const message_view<>& m, Emit&& emit) {
        const std::string_view t = m.msg_type();
        if (t == "D") new_order(session, m, emit);
        else if (t == "F") cancel(session, m, emit);
        else if (t == "G") replace(session, m, emit);
    }

    template <class Emit>
    void on_disconnect(int session, Emit&& emit) {
        if (!cfg_.cancel_on_disconnect) return;
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            order& o = orders_[i];
            if (o.session != session || !o.live) continue;
            unrest(i);
            o.live = false;
            o.leaves = 0;
            o.status = ord_status::canceled;
            report(o, '4', 0, 0, "", emit);  // unsolicited (E.1.b): no OrigClOrdID
        }
    }

    // End of the trading day: every live day order is done (A.1.b).
    template <class Emit>
    void end_of_day(Emit&& emit) {
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            order& o = orders_[i];
            if (!o.live || o.tif != '0') continue;
            unrest(i);
            o.live = false;
            o.leaves = 0;
            o.status = ord_status::done_for_day;
            if (o.session >= 0) report(o, '3', 0, 0, "", emit);
        }
    }

private:
    struct order {
        int session = -1;
        std::string cl_ord_id, order_id, symbol;
        char side = '1', ord_type = '2', tif = '0', status = ord_status::new_;
        fixed price = 0, order_qty = 0, cum = 0, leaves = 0, value = 0;  // value: sum of fills' price x qty, for AvgPx
        std::uint64_t priority = 0;
        bool live = false;
    };
    struct side_book {
        std::vector<std::size_t> bids, asks;  // indexes into orders_, best first
    };

    static std::string num(fixed v) {
        char b[48];
        const fixed w = v / fixed_one, f = (v < 0 ? -v : v) % fixed_one;
        if (f == 0) std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(w));
        else {
            std::snprintf(b, sizeof(b), "%s%lld.%08lld", v < 0 && w == 0 ? "-" : "", static_cast<long long>(w), static_cast<long long>(f));
            std::size_t n = std::char_traits<char>::length(b);
            while (b[n - 1] == '0') b[--n] = 0;
        }
        return b;
    }
    static std::string field(std::uint32_t tag, std::string_view v) { return std::to_string(tag) + "=" + std::string(v) + "\x01"; }
    std::string next_order_id() { return "V" + std::to_string(++order_ids_); }
    fixed avg_px(const order& o) const { return o.cum == 0 ? 0 : static_cast<fixed>(static_cast<sim_wide>(o.value) * fixed_one / o.cum); }

    template <class Emit>
    void report(const order& o, char exec_type, fixed last_qty, fixed last_px, std::string_view orig, Emit& emit,
                std::string_view cl_ord_id = {}, std::string_view extra = {}) {
        if (o.session < 0) return;
        std::string b = field(37, o.order_id) + field(11, cl_ord_id.empty() ? std::string_view(o.cl_ord_id) : cl_ord_id);
        if (!orig.empty()) b += field(41, orig);
        b += field(17, "VE" + std::to_string(++exec_ids_)) + field(150, std::string(1, exec_type)) +
             field(39, std::string(1, o.status)) + field(55, o.symbol) + field(54, std::string(1, o.side)) +
             field(38, num(o.order_qty)) + field(40, std::string(1, o.ord_type));
        if (o.ord_type != '1') b += field(44, num(o.price));
        if (exec_type == 'F') b += field(32, num(last_qty)) + field(31, num(last_px));
        b += field(151, num(o.leaves)) + field(14, num(o.cum)) + field(6, num(avg_px(o)));
        b += extra;
        emit(o.session, "8", b);
    }
    template <class Emit>
    void cancel_reject(int session, std::string_view id, std::string_view orig, const order* o, char response_to, int reason,
                       Emit& emit) {
        std::string b = field(37, o ? std::string_view(o->order_id) : std::string_view("NONE")) + field(11, id) + field(41, orig) +
                        field(39, std::string(1, o ? o->status : ord_status::rejected)) + field(434, std::string(1, response_to)) +
                        field(102, std::to_string(reason));
        emit(session, "9", b);
    }

    std::size_t* find(int session, std::string_view cl_ord_id) {
        const auto it = ids_.find({session, std::string(cl_ord_id)});
        return it == ids_.end() ? nullptr : &it->second;
    }

    template <class Emit>
    void new_order(int session, const message_view<>& m, Emit& emit) {
        const std::string id(m.get(11).value_or(""));
        if (std::size_t* known = find(session, id)) {
            order& o = orders_[*known];
            const bool poss = m.get(97) == std::string_view("Y") || m.get(43) == std::string_view("Y");
            if (poss) report(o, 'I', 0, 0, "", emit);  // F.1.b: already received: its current state
            else report(o, '8', 0, 0, "", emit, {}, field(103, "6"));  // F.1.a: duplicate, the order's own status
            return;
        }
        order o;
        o.session = session;
        o.cl_ord_id = id;
        o.order_id = next_order_id();
        o.symbol = std::string(m.get(55).value_or(""));
        o.side = m.get(54).value_or("1")[0];
        o.ord_type = m.get(40).value_or("1")[0];
        o.tif = m.get(59).value_or("0")[0];
        o.price = detail::parse_fixed(m.get(44).value_or("0")).value_or(0);
        o.order_qty = o.leaves = detail::parse_fixed(m.get(38).value_or("0")).value_or(0);
        o.status = ord_status::new_;
        o.priority = ++clock_;
        o.live = true;
        orders_.push_back(o);
        const std::size_t i = orders_.size() - 1;
        ids_[{session, id}] = i;
        report(orders_[i], '0', 0, 0, "", emit);
        if (orders_[i].tif == '4' && available(orders_[i]) < orders_[i].leaves) {  // FOK that cannot fill (I.1.a)
            kill(i, emit);
            return;
        }
        match(i, emit);
        order& n = orders_[i];
        if (!n.live) return;
        if (n.tif == '3' || n.ord_type == '1') kill(i, emit);  // IOC or market: what did not fill is canceled (I.1.b)
        else rest(i);
    }

    template <class Emit>
    void cancel(int session, const message_view<>& m, Emit& emit) {
        const std::string id(m.get(11).value_or("")), orig(m.get(41).value_or(""));
        std::size_t* at = find(session, orig);
        if (!at) return cancel_reject(session, id, orig, nullptr, '1', 1, emit);  // B.1.f: unknown order
        order& o = orders_[*at];
        if (!o.live) return cancel_reject(session, id, orig, &o, '1', 0, emit);  // too late to cancel
        ids_[{session, id}] = *at;
        if (cfg_.pending_acks) {
            const char was = o.status;
            o.status = ord_status::pending_cancel;
            report(o, '6', 0, 0, orig, emit, id);
            o.status = was;
        }
        unrest(*at);
        o.live = false;
        o.leaves = 0;
        o.status = ord_status::canceled;
        o.cl_ord_id = id;
        report(o, '4', 0, 0, orig, emit);
    }

    template <class Emit>
    void replace(int session, const message_view<>& m, Emit& emit) {
        const std::string id(m.get(11).value_or("")), orig(m.get(41).value_or(""));
        std::size_t* at = find(session, orig);
        if (!at) return cancel_reject(session, id, orig, nullptr, '2', 1, emit);
        const std::size_t i = *at;
        order& o = orders_[i];
        const fixed qty = detail::parse_fixed(m.get(38).value_or("0")).value_or(0);
        // A filled order may be reopened by raising its quantity (C.1.c); any other finished one is too late.
        if (!o.live && !(o.status == ord_status::filled && qty > o.cum)) return cancel_reject(session, id, orig, &o, '2', 0, emit);
        ids_[{session, id}] = i;
        o.live = true;
        const fixed price = detail::parse_fixed(m.get(44).value_or("0")).value_or(o.price);
        if (cfg_.pending_acks) {
            const char was = o.status;
            o.status = ord_status::pending_replace;
            report(o, 'E', 0, 0, orig, emit, id);
            o.status = was;
        }
        const bool keeps_priority = price == o.price && qty <= o.order_qty;
        unrest(i);
        o.cl_ord_id = id;
        if (qty <= o.cum) {  // C.3.b, C.3.c: amended to what has filled, so done
            o.order_qty = o.cum;
            o.leaves = 0;
            o.live = false;
            o.status = ord_status::filled;
            report(o, '5', 0, 0, orig, emit);
            return;
        }
        o.order_qty = qty;
        o.leaves = qty - o.cum;
        o.price = price;
        if (!keeps_priority) o.priority = ++clock_;
        o.status = o.cum > 0 ? ord_status::partially_filled : ord_status::new_;
        report(o, '5', 0, 0, orig, emit);
        match(i, emit);
        if (orders_[i].live) rest(i);
    }

    bool crosses(const order& taker, const order& maker) const {
        if (taker.ord_type == '1') return true;
        return taker.side == '1' ? maker.price <= taker.price : maker.price >= taker.price;
    }
    std::vector<std::size_t>& opposite(const order& o) {
        side_book& b = books_[o.symbol];
        return o.side == '1' ? b.asks : b.bids;
    }
    fixed available(const order& o) {
        fixed n = 0;
        for (const std::size_t k : opposite(o))
            if (crosses(o, orders_[k])) n += orders_[k].leaves;
        return n;
    }
    template <class Emit>
    void match(std::size_t i, Emit& emit) {
        std::vector<std::size_t>& book = opposite(orders_[i]);
        while (orders_[i].leaves > 0 && !book.empty() && crosses(orders_[i], orders_[book.front()])) {
            const std::size_t k = book.front();
            order& taker = orders_[i];
            order& maker = orders_[k];
            const fixed q = std::min(taker.leaves, maker.leaves), px = maker.price;
            for (order* x : {&taker, &maker}) {
                x->leaves -= q;
                x->cum += q;
                x->value += static_cast<fixed>(static_cast<sim_wide>(q) * px / fixed_one);
                x->status = x->leaves == 0 ? ord_status::filled : ord_status::partially_filled;
                if (x->leaves == 0) x->live = false;
            }
            if (maker.leaves == 0) book.erase(book.begin());
            report(maker, 'F', q, px, "", emit);
            report(taker, 'F', q, px, "", emit);
        }
    }
    template <class Emit>
    void kill(std::size_t i, Emit& emit) {
        order& o = orders_[i];
        o.live = false;
        o.leaves = 0;
        o.status = ord_status::canceled;
        report(o, '4', 0, 0, "", emit);
    }
    void rest(std::size_t i) {
        const order& o = orders_[i];
        side_book& b = books_[o.symbol];
        std::vector<std::size_t>& v = o.side == '1' ? b.bids : b.asks;
        const auto better = [&](std::size_t a, std::size_t c) {
            const order &x = orders_[a], &y = orders_[c];
            if (x.price != y.price) return o.side == '1' ? x.price > y.price : x.price < y.price;
            return x.priority < y.priority;
        };
        v.insert(std::upper_bound(v.begin(), v.end(), i, better), i);
    }
    void unrest(std::size_t i) {
        side_book& b = books_[orders_[i].symbol];
        for (auto* v : {&b.bids, &b.asks}) v->erase(std::remove(v->begin(), v->end(), i), v->end());
    }

    venue_config cfg_;
    std::vector<order> orders_;
    std::map<std::pair<int, std::string>, std::size_t> ids_;  // (session, ClOrdID) to order, the chain included
    std::map<std::string, side_book, std::less<>> books_;
    std::uint64_t clock_ = 0, order_ids_ = 0, exec_ids_ = 0;
};

}  // namespace fixgw
