// Subjects for check_hot_path.py: the order book (note 007, S4), in a file of
// its own so the codec's subjects keep the inlining a caller of theirs alone
// would get (inlining is decided per file). Spec: docs/design/007-order-entry.md
#include <fixgw/orders.hpp>

// The order book (note 007, S4): a request checked and recorded, a report
// applied, the stale-order rule.
extern "C" [[gnu::noinline]] bool fixgw_book_outbound(fixgw::order_book& b, const char* body, std::size_t n,
                                                      fixgw::session_time now) {
    const std::string_view m(body, n);
    if (!b.check("D", m, now).ok) return false;
    b.record("D", m, now);
    return true;
}
extern "C" [[gnu::noinline]] bool fixgw_book_inbound(fixgw::order_book& b, const fixgw::message_view<>& m,
                                                     fixgw::order_event& e) {
    return b.inbound(m, e);
}
extern "C" [[gnu::noinline]] bool fixgw_book_may_resend(fixgw::order_book& b, const char* id, std::size_t n,
                                                        fixgw::session_time now, fixgw::order_event& e) {
    return b.may_resend("D", std::string_view(id, n), now, e);
}

