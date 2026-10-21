// libFuzzer target for the framer and decoder: the code that faces untrusted
// bytes. Spec: docs/design/002-codec.md (S7)
//
// The input is a byte stream, framed as the transport would frame it; every
// frame accepted is decoded. Beyond what ASan and UBSan catch, it checks:
// a frame never claims more bytes than it was given; a garbled verdict
// discards at least one byte; every decoded field lies inside its frame; an
// accepted message ends in CheckSum (10).
#include <fixgw/decode.hpp>
#include <fixgw/framing.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const char* p = reinterpret_cast<const char*>(data);
    std::size_t at = 0;
    fixgw::message_view<> v;
    while (at < size) {
        const auto r = fixgw::frame(p + at, size - at, 4096);
        if (r.status == fixgw::frame_status::need_more) break;
        if (r.length == 0 || r.length > size - at) std::abort();
        if (r.status == fixgw::frame_status::complete) {
            const auto d = fixgw::decode(p + at, r.length, v);
            if (d.ok) {
                if (v.size() < 4 || v[v.size() - 1].tag != 10) std::abort();
                for (std::size_t i = 0; i < v.size(); ++i) {
                    const auto& f = v[i];
                    if (std::size_t{f.offset} + f.length > r.length) std::abort();
                    volatile char sink = f.length ? v.value(i).back() : 0;
                    (void)sink;
                }
            }
        }
        at += r.length;
    }
    return 0;
}
