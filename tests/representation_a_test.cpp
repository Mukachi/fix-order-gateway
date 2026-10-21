// Representation A (bench/representation_a.hpp) agrees with B on content, so
// the measured comparison is between equals. Spec: docs/design/002-codec.md (S8)
#include "../bench/representation_a.hpp"
#include "fix_text.hpp"

#include <fixgw/decode.hpp>
#include <fixgw/encode.hpp>

int main() {
    using namespace fixgw;
    using fixgw::test::check;
    const std::string m = fixgw::test::message(fixgw::test::new_order_body(7));
    std::unordered_map<std::uint32_t, std::string> a;
    message_view<> b;
    bool same = bench::decode_a(m.data(), m.size(), a) && decode(m.data(), m.size(), b).ok && a.size() == b.size();
    for (std::size_t i = 0; same && i < b.size(); ++i) same = a.count(b[i].tag) && a[b[i].tag] == b.value(i);
    check(same, "A and B decode the same fields with the same values");

    bench::message_a e("D");
    e.set(49, std::string("CLIENT"));
    e.set(56, std::string("VENUE"));
    e.set(34, std::uint64_t{7});
    e.set(52, std::string("20261021-09:30:00.123"));
    e.set(11, std::string("ORD-000001"));
    e.set(1, std::string("ACC1"));
    e.set(55, std::string("VOD.L"));
    e.set(54, '1');
    e.set(60, std::string("20261021-09:30:00.123"));
    e.set(38, std::uint64_t{1000});
    e.set(40, '2');
    e.set(44, std::string("72.53"));
    e.set(59, '0');
    check(e.serialise() == m, "A encodes the same bytes as B and the test's own framing");
    return fixgw::test::result();
}
