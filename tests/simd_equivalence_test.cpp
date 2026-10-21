// The SIMD paths against the scalar ones (note 009): no read outside the
// buffer at any message length (S3, meaningful under ASan), and the same
// result on mutated input (S1, S2).
#include "fix_text.hpp"

#include <fixgw/decode.hpp>
#include <fixgw/framing.hpp>

#include <random>
#include <string>
#include <vector>

namespace {

using namespace fixgw;
using fixgw::test::check;

bool same(const decode_result& a, const message_view<>& va, const decode_result& b, const message_view<>& vb) {
    if (a.ok != b.ok || a.reject_reason != b.reject_reason || a.ref_tag != b.ref_tag) return false;
    if (!a.ok) return true;
    if (va.size() != vb.size()) return false;
    for (std::size_t i = 0; i < va.size(); ++i)
        if (va[i].tag != vb[i].tag || va[i].offset != vb[i].offset || va[i].length != vb[i].length) return false;
    return true;
}

}  // namespace

int main() {
    // S3: lengths covering every end position in a 32-byte block, twice over.
    // Unpadded paths get an allocation of exactly the message's size; the
    // padded path exactly its 32-byte contract.
    {
        bool ok = true;
        for (int k = 0; k < 64; ++k) {
            const std::string m = fixgw::test::message("35=0|49=C|56=V|34=1|58=" + std::string(1 + k, 'x') + "|");
            const std::size_t n = m.size();
            std::vector<char> exact(m.begin(), m.end());        // exactly the message: ASan sees any over-read
            std::vector<char> padded(m.begin(), m.end());
            padded.resize(n + 64, soh);                          // SOH past the end must not be seen
            message_view<> a, b, c;
            const auto fs = frame_scalar(exact.data(), n, 4096), fv = frame_simd(exact.data(), n, 4096);
            const auto ra = decoder<256, scan::scalar>::decode(exact.data(), n, a);
            const auto rb = decoder<256, scan::simd<false>>::decode(exact.data(), n, b);
            const auto rc = decoder<256, scan::simd<true>>::decode(padded.data(), n, c);
            static structural_index<64> ix;
            message_view<> d, e;
            const bool built = ix.build<false>(exact.data(), n);
            const auto rd = decode_indexed(exact.data(), ix, 0, n, d);
            const bool built_p = ix.build<true>(padded.data(), n);
            const auto re = decode_indexed(padded.data(), ix, 0, n, e);
            ok = ok && built && built_p && same(ra, a, rd, d) && same(ra, a, re, e);
            ok = ok && fs.status == frame_status::complete && fv.status == fs.status && fv.length == fs.length &&
                 ra.ok && same(ra, a, rb, b) && same(ra, a, rc, c);
        }
        check(ok, "S3 64 consecutive message lengths: SIMD and the index read nothing past an exact allocation, "
                  "padded paths nothing past their padding, SOH beyond the end ignored, results equal scalar");
    }

    // S1, S2: 100,000 mutations of a NewOrderSingle: framing and decoding agree.
    {
        const std::string base = fixgw::test::message(fixgw::test::new_order_body(7));
        std::mt19937_64 rng(5);
        const char alphabet[] = {'\x01', '=', '0', '9', 'A', '8', '1', '\x7f'};
        std::size_t differ = 0, complete = 0;
        for (int i = 0; i < 100'000; ++i) {
            std::string m = base;
            const int edits = 1 + static_cast<int>(rng() % 4);
            for (int e = 0; e < edits; ++e) {
                const std::size_t at = rng() % m.size();
                switch (rng() % 3) {
                    case 0: m[at] = alphabet[rng() % sizeof(alphabet)]; break;
                    case 1: m = m.substr(0, at) + alphabet[rng() % sizeof(alphabet)] + m.substr(at); break;
                    default: m.erase(at, 1); break;
                }
            }
            const std::vector<char> exact(m.begin(), m.end());
            const auto fs = frame_scalar(exact.data(), m.size(), 4096), fv = frame_simd(exact.data(), m.size(), 4096);
            if (fs.status != fv.status || fs.length != fv.length) ++differ;
            // Decode whatever framing accepts, and also the raw bytes, which
            // reach the decoder's error paths more often.
            message_view<> a, b, c;
            const auto ra = decoder<256, scan::scalar>::decode(exact.data(), m.size(), a);
            const auto rb = decoder<256, scan::simd<false>>::decode(exact.data(), m.size(), b);
            static structural_index<64> ix;
            ix.build<false>(exact.data(), m.size());
            const auto rc = decode_indexed(exact.data(), ix, 0, m.size(), c);
            if (!same(ra, a, rb, b) || !same(ra, a, rc, c)) ++differ;
            complete += fs.status == frame_status::complete;
        }
        check(differ == 0, "S1 S2 100,000 mutated messages: scalar, SIMD and the index frame and decode identically (" +
                               std::to_string(complete) + " still framed whole, " + std::to_string(differ) + " differences)");
    }
    // Burst: one index over 1,000 coalesced messages of mixed kinds; each
    // message framed and decoded from it, against the scalar decoder alone.
    {
        std::string stream;
        for (int i = 0; i < 1000; ++i)
            stream += fixgw::test::message(i % 3 == 0 ? fixgw::test::new_order_body(i)
                                         : i % 3 == 1 ? "35=0|49=V|56=C|34=" + std::to_string(i) + "|52=20261021-09:30:01.000|"
                                                      : "35=A|49=C|56=V|34=1|52=20261021-09:30:00.000|98=0|108=30|95=5|96=ab|cd|");
        std::vector<char> buf(stream.begin(), stream.end());
        static structural_index<4096> ix;
        const bool built = ix.build<false>(buf.data(), buf.size());
        std::size_t at = 0, n = 0, agree = 0;
        while (at < buf.size()) {
            const auto f = frame_simd(buf.data() + at, buf.size() - at, 4096);
            if (f.status != frame_status::complete) break;
            message_view<> a, b;
            const auto ra = decoder<256, scan::scalar>::decode(buf.data() + at, f.length, a);
            const auto rb = decode_indexed(buf.data(), ix, at, f.length, b);
            agree += ra.ok && same(ra, a, rb, b) && b.bytes().data() == buf.data() + at;
            at += f.length;
            ++n;
        }
        check(built && n == 1000 && agree == 1000,
              "burst: one index over 1,000 coalesced messages; every message decodes from it as the scalar decoder does");
    }
    return fixgw::test::result();
}
