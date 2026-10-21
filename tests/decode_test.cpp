// Tests for include/fixgw/decode.hpp. Spec: docs/design/002-codec.md (S3, S4)
#include "fix_text.hpp"

#include <fixgw/decode.hpp>

#include <string>

namespace {

using namespace fixgw;
using fixgw::test::check;

// Decodes a whole message built by fix_text (which frames it correctly).
decode_result run(const std::string& body, message_view<>& v, std::string& keep) {
    keep = fixgw::test::message(body);
    return decode(keep.data(), keep.size(), v);
}

void rejects(const std::string& body, int reason, std::uint32_t tag, const std::string& name) {
    message_view<> v;
    std::string keep;
    const auto r = run(body, v, keep);
    check(!r.ok && r.reject_reason == reason && r.ref_tag == tag,
          "S3 " + name + ": reason " + std::to_string(r.reject_reason) + ", tag " + std::to_string(r.ref_tag) +
              (r.why ? std::string(" (") + r.why + ")" : ""));
}

constexpr bool header_bits_distinct() {
    constexpr std::uint32_t tags[] = {8, 9, 35, 49, 56, 115, 128, 90, 91, 34, 50, 142, 57, 143,
                                      116, 144, 129, 145, 43, 97, 52, 122, 212, 213, 347, 369, 627};
    for (std::uint32_t a : tags) {
        if (detail::header_index(a) >= 63) return false;
        for (std::uint32_t b : tags)
            if (a != b && detail::header_index(a) == detail::header_index(b)) return false;
    }
    return true;
}
static_assert(header_bits_distinct(), "every non-group header tag has its own bit");

}  // namespace

int main() {
    {
        message_view<> v;
        std::string keep;
        const auto r = run(fixgw::test::new_order_body(5), v, keep);
        check(r.ok && v.size() == 17, "a NewOrderSingle decodes: 17 fields (" + std::to_string(v.size()) + ")");
        check(v.msg_type() == "D" && v.get(11) == "ORD-000001" && v.get(44) == "72.53" && v.get(34) == "5",
              "fields read in place: MsgType, ClOrdID, Price, MsgSeqNum");
        check(v[0].tag == 8 && v[1].tag == 9 && v[v.size() - 1].tag == 10 && v.value(v.size() - 1).size() == 3,
              "fields in order, BeginString first, CheckSum last");
        check(!v.get(9999).has_value(), "an absent tag reads as absent");
    }
    {
        message_view<> v;
        std::string keep;
        const auto r = run("35=A|49=C|56=V|34=1|52=20261021-09:30:00|98=0|108=30|95=5|96=ab|cd|", v, keep);
        check(r.ok && v.get(96) == std::string_view("ab\x01" "cd", 5), "S4 RawData containing SOH is read whole, by its length");
    }
    {
        message_view<> v;
        std::string keep;
        const auto r = run("35=0|49=C|56=V|627=2|628=H1|629=20261021-09:30:00|628=H2|34=1|52=20261021-09:30:00|", v, keep);
        check(r.ok, "the NoHops group's fields may repeat in the header");
    }

    rejects("35=0|49=C|0=x|", reject_reason::invalid_tag_number, 0, "tag 0");
    rejects("35=0|49=C|012=x|", reject_reason::invalid_tag_number, 0, "tag with a leading zero");
    rejects("35=0|49=C|4a=x|", reject_reason::invalid_tag_number, 0, "tag not a number");
    rejects("35=0|49=C|abc|", reject_reason::invalid_tag_number, 0, "field without =");
    rejects("35=0|49=C|58=|", reject_reason::tag_without_value, 58, "tag without a value");
    rejects("35=D|49=C|11=X|56=V|", reject_reason::tag_out_of_order, 56, "header tag after the body");
    rejects("35=0|49=C|49=D|", reject_reason::tag_repeated, 49, "header tag repeated");
    rejects("35=0|49=C|93=2|89=ab|58=x|", reject_reason::tag_out_of_order, 58, "body tag after the trailer");
    rejects("35=A|49=C|95=5|58=x|", reject_reason::incorrect_data_format, 58, "data length not followed by its data");
    rejects("35=A|49=C|95=9|96=abc|", reject_reason::incorrect_data_format, 96, "data shorter than its length");
    rejects("35=A|49=C|95=x|96=abc|", reject_reason::incorrect_data_format, 95, "data length not a number");
    rejects("35=A|49=C|95=10|96=abc|", reject_reason::incorrect_data_format, 0, "data length reaching over the trailer");
    {
        message_view<4> v;
        const std::string m = fixgw::test::message("35=0|49=C|56=V|");
        const auto r = decoder<4>::decode(m.data(), m.size(), v);
        check(!r.ok && r.reject_reason == reject_reason::other, "more fields than the decoder holds: refused, not overrun");
    }
    return fixgw::test::result();
}
