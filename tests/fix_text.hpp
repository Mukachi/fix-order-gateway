// Test helpers: FIX messages written readably, with '|' for SOH, and framed
// here independently of the code under test (its own BodyLength and CheckSum).
#pragma once

#include <cstdio>
#include <string>

namespace fixgw::test {

inline int failures = 0;

inline void check(bool cond, const std::string& what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

inline int result() {
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

// '|' becomes SOH.
inline std::string soh(std::string s) {
    for (auto& c : s)
        if (c == '|') c = '\x01';
    return s;
}

// A whole message: "8=FIX.4.4|9=<len>|" + body + "10=<sum>|". `body` starts
// with 35= and ends with '|'. `body_length_delta` and `checksum_delta` corrupt
// the frame on purpose.
inline std::string message(const std::string& body, int body_length_delta = 0, int checksum_delta = 0) {
    const std::string b = soh(body);
    std::string m = "8=FIX.4.4\x01" "9=" + std::to_string(static_cast<int>(b.size()) + body_length_delta) + "\x01" + b;
    unsigned sum = 0;
    for (unsigned char c : m) sum += c;
    char t[16];
    std::snprintf(t, sizeof(t), "10=%03u\x01", (sum + static_cast<unsigned>(checksum_delta)) % 256);
    return m + t;
}

// A NewOrderSingle body, as an order path would send it.
inline std::string new_order_body(int seq = 1) {
    return "35=D|49=CLIENT|56=VENUE|34=" + std::to_string(seq) +
           "|52=20261021-09:30:00.123456|11=ORD-000001|1=ACC1|55=VOD.L|54=1|"
           "60=20261021-09:30:00.123456|38=1000|40=2|44=72.53|59=0|";
}

}  // namespace fixgw::test
