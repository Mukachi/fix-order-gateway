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
            padded.resize(n + 32, soh);                          // SOH past the end must not be seen
            message_view<> a, b, c;
            const auto fs = frame_scalar(exact.data(), n, 4096), fv = frame_simd(exact.data(), n, 4096);
            const auto ra = decoder<256, scan::scalar>::decode(exact.data(), n, a);
            const auto rb = decoder<256, scan::simd<false>>::decode(exact.data(), n, b);
            const auto rc = decoder<256, scan::simd<true>>::decode(padded.data(), n, c);
            ok = ok && fs.status == frame_status::complete && fv.status == fs.status && fv.length == fs.length &&
                 ra.ok && same(ra, a, rb, b) && same(ra, a, rc, c);
        }
        check(ok, "S3 64 consecutive message lengths: SIMD reads nothing past an exact allocation, padded "
                  "nothing past its 32 bytes, SOH beyond the end ignored, results equal scalar");
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
            message_view<> a, b;
            const auto ra = decoder<256, scan::scalar>::decode(exact.data(), m.size(), a);
            const auto rb = decoder<256, scan::simd<false>>::decode(exact.data(), m.size(), b);
            if (!same(ra, a, rb, b)) ++differ;
            complete += fs.status == frame_status::complete;
        }
        check(differ == 0, "S1 S2 100,000 mutated messages: scalar and SIMD frame and decode identically (" +
                               std::to_string(complete) + " still framed whole, " + std::to_string(differ) + " differences)");
    }
    return fixgw::test::result();
}
