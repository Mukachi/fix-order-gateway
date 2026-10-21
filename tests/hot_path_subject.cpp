// Subjects for check_hot_path.py, which disassembles this binary: the codec's
// hot paths behind fixed, unmangled symbols. Spec: docs/design/002-codec.md (S6)
#include <fixgw/decode.hpp>
#include <fixgw/encode.hpp>
#include <fixgw/framing.hpp>

#include <cstdio>

// Each implementation behind its own symbol; `flatten` inlines what each
// calls, so the check sees the whole path (note 009).
extern "C" [[gnu::noinline, gnu::flatten]] fixgw::frame_result fixgw_frame(const char* p, std::size_t n) {
    return fixgw::frame_scalar(p, n, 4096);
}
extern "C" [[gnu::noinline, gnu::flatten]] fixgw::frame_result fixgw_frame_simd(const char* p, std::size_t n) {
    return fixgw::frame_simd(p, n, 4096);
}

extern "C" [[gnu::noinline, gnu::flatten]] fixgw::decode_result fixgw_decode(const char* p, std::size_t n,
                                                                            fixgw::message_view<>& v) {
    return fixgw::decoder<256, fixgw::scan::scalar>::decode(p, n, v);
}
extern "C" [[gnu::noinline, gnu::flatten]] fixgw::decode_result fixgw_decode_simd(const char* p, std::size_t n,
                                                                                 fixgw::message_view<>& v) {
    return fixgw::decoder<256, fixgw::scan::simd<false>>::decode(p, n, v);
}
extern "C" [[gnu::noinline, gnu::flatten]] fixgw::decode_result fixgw_decode_simd_padded(const char* p, std::size_t n,
                                                                                        fixgw::message_view<>& v) {
    return fixgw::decoder<256, fixgw::scan::simd<true>>::decode(p, n, v);
}

// Encoder B: a NewOrderSingle, field by field.
extern "C" [[gnu::noinline]] std::size_t fixgw_encode_b(char* buf, std::size_t cap, std::uint64_t seq, std::uint64_t qty,
                                                        std::int64_t price) {
    fixgw::encoder e(buf, cap);
    e.begin("D");
    e.add(49, "CLIENT");
    e.add(56, "VENUE");
    e.add(34, seq);
    e.add(52, "20261021-09:30:00.123");
    e.add(11, "ORD-000001");
    e.add(1, "ACC1");
    e.add(55, "VOD.L");
    e.add(54, '1');
    e.add(60, "20261021-09:30:00.123");
    e.add(38, qty);
    e.add(40, '2');
    e.add_decimal(44, price, 2);
    e.add(59, '0');
    return e.finish().size();
}

// Template C: one order.
extern "C" [[gnu::noinline]] std::size_t fixgw_encode_c(fixgw::new_order_template& t, std::uint64_t seq,
                                                        std::int64_t now_ms, std::uint64_t id, std::uint64_t qty,
                                                        std::uint64_t price) {
    return t.fill(seq, now_ms, id, qty, price).size();
}

int main(int argc, char**) {
    char buf[512];
    const std::size_t n = fixgw_encode_b(buf, sizeof(buf), static_cast<std::uint64_t>(argc), 100, 7253);
    fixgw::message_view<> v;
    const bool ok = fixgw_frame(buf + (sizeof(buf) - n), 0).status != fixgw::frame_status::complete &&
                    fixgw_frame_simd(buf, 0).status != fixgw::frame_status::complete &&
                    fixgw_decode(buf, 0, v).ok == false && fixgw_decode_simd(buf, 0, v).ok == false &&
                    fixgw_decode_simd_padded(buf, 0, v).ok == false;
    static fixgw::new_order_template t({"CLIENT", "VENUE", "ACC1", "VOD.L", '1', '2', '0', 2});
    std::printf("%zu %zu %d\n", n, fixgw_encode_c(t, 1, 0, 1, 1, 1), ok);
    return 0;
}
