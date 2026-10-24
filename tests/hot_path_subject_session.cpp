// Subjects for check_hot_path.py: the session (note 003, S6) and the journal
// (note 004, S6), in a file of their own so the codec's subjects keep the
// inlining a caller of theirs alone would get. Spec: docs/design/003-session.md
#include <fixgw/durability.hpp>
#include <fixgw/journal.hpp>
#include <fixgw/session.hpp>

struct quiet_sink {
    std::size_t bytes = 0, delivered = 0;
    void write(std::string_view b) { bytes += b.size(); }
    void disconnect() {}
    void deliver(const fixgw::message_view<>&) { ++delivered; }
    void log(fixgw::session_log, std::string_view, std::uint64_t) {}
    bool resend(const fixgw::message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};
struct quiet_store {
    void record(std::uint64_t, std::string_view) {}
    std::string_view lookup(std::uint64_t) { return {}; }
    void reset() {}
};
using hot_session = fixgw::session<quiet_sink, quiet_store>;
extern "C" [[gnu::noinline]] std::size_t fixgw_session_inbound(hot_session& s, const char* p, std::size_t n,
                                                               fixgw::session_time now) {
    return s.on_bytes(p, n, now);
}
extern "C" [[gnu::noinline]] int fixgw_session_send(hot_session& s, const char* body, std::size_t n,
                                                    fixgw::session_time now) {
    return static_cast<int>(s.send("D", std::string_view(body, n), now));
}

// The journal (note 004, S6): appending a record to a file (A, B and D
// append so) and to a mapping (C).
extern "C" [[gnu::noinline]] bool fixgw_journal_append(fixgw::journal_file& f, const char* p, std::size_t n,
                                                       fixgw::session_time t) {
    return f.append(fixgw::record_kind::bytes, t, std::string_view(p, n));
}
extern "C" [[gnu::noinline]] bool fixgw_mapped_append(fixgw::mapped_journal& j, const char* p, std::size_t n,
                                                      fixgw::session_time t) {
    return j.append(fixgw::record_kind::bytes, t, std::string_view(p, n), {});
}

