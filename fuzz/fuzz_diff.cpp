// Differential fuzz target (note 009, S1 S2): the same bytes through every
// framer and decoder; any disagreement with the scalar path aborts. The
// unpadded paths read the input in place (ASan sees any over-read); the padded
// paths read a copy with 64 bytes of padding.
#include <fixgw/decode.hpp>
#include <fixgw/framing.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using namespace fixgw;

bool same(const decode_result& a, const message_view<>& va, const decode_result& b, const message_view<>& vb) {
    if (a.ok != b.ok || a.reject_reason != b.reject_reason || a.ref_tag != b.ref_tag) return false;
    if (!a.ok) return true;
    if (va.size() != vb.size()) return false;
    for (std::size_t i = 0; i < va.size(); ++i)
        if (va[i].tag != vb[i].tag || va[i].offset != vb[i].offset || va[i].length != vb[i].length) return false;
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const char* p = reinterpret_cast<const char*>(data);
    static structural_index<160> ix;  // 10 KB, above the 8 KB input limit
    static std::vector<char> padded;
    padded.assign(p, p + size);
    padded.resize(size + 64, '\x01');
    message_view<> a, b, c, d, e, f;

    // The whole input decoded as one message, by each decoder: reaches the
    // error paths far more often than framed input does.
    if (size <= 160 * 64) {
        const auto ra = decoder<256, scan::scalar>::decode(p, size, a);
        if (!same(ra, a, decoder<256, scan::simd<false>>::decode(p, size, b), b)) std::abort();
        if (!same(ra, a, decoder<256, scan::simd<true>>::decode(padded.data(), size, c), c)) std::abort();
        if (!ix.build<false>(p, size) || !same(ra, a, decode_indexed(p, ix, 0, size, d), d)) std::abort();
        if (!ix.build<true>(padded.data(), size) || !same(ra, a, decode_indexed(padded.data(), ix, 0, size, e), e))
            std::abort();
    }
    // As a stream: both framers agree; each frame decoded from one shared index.
    if (size <= 160 * 64 && ix.build<false>(p, size)) {
        std::size_t at = 0;
        while (at < size) {
            const auto fs = frame_scalar(p + at, size - at, 4096), fv = frame_simd(p + at, size - at, 4096);
            if (fs.status != fv.status || fs.length != fv.length) std::abort();
            if (fs.status == frame_status::need_more) break;
            if (fs.status == frame_status::complete) {
                const auto ra = decoder<256, scan::scalar>::decode(p + at, fs.length, a);
                if (!same(ra, a, decode_indexed(p, ix, at, fs.length, f), f)) std::abort();
            }
            at += fs.length;
        }
    }
    return 0;
}
