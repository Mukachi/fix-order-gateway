// The messages stage_bench and qf_bench's parse mode measure (note 008, D6):
// the same bytes for both. ExecutionReports from the venue to the gateway,
// each different (MsgSeqNum from 2, ExecID unique, ClOrdID, quantities and
// prices varied), as a gateway receives them; and NewOrderSingle bodies as the
// application submits them. Deterministic: the same seed each run.
#pragma once

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace stage {

inline constexpr std::size_t count = 110'000;  // 10,000 warm-up, 100,000 measured
inline constexpr std::size_t warmup = 10'000;

inline std::string framed(const std::string& body) {
    std::string m = "8=FIX.4.4\x01" "9=" + std::to_string(body.size()) + "\x01" + body;
    unsigned sum = 0;
    for (const unsigned char c : m) sum += c;
    char t[16];
    std::snprintf(t, sizeof(t), "10=%03u\x01", sum % 256);
    return m + t;
}

// ExecutionReport New for order i, MsgSeqNum i + 2 (1 is the venue's Logon).
inline std::vector<std::string> reports() {
    std::mt19937_64 rng(8);
    std::vector<std::string> v;
    v.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::string id = std::to_string(1'000'000 + i), qty = std::to_string(100 * (1 + rng() % 50)),
                          px = std::to_string(70 + rng() % 5) + "." + std::to_string(10 + rng() % 90);
        v.push_back(framed("35=8\x01" "49=VENUE\x01" "56=CLIENT\x01" "34=" + std::to_string(i + 2) +
                           "\x01" "52=20261024-09:30:00.123\x01" "37=V" + id + "\x01" "11=" + id + "\x01" "17=E" + id +
                           "\x01" "150=0\x01" "39=0\x01" "55=VOD.L\x01" "54=1\x01" "38=" + qty + "\x01" "40=2\x01" "44=" + px +
                           "\x01" "151=" + qty + "\x01" "14=0\x01" "6=0\x01" "60=20261024-09:30:00.120\x01"));
    }
    return v;
}

// NewOrderSingle bodies for order i (the ClOrdIDs the reports answer).
inline std::vector<std::string> order_bodies() {
    std::mt19937_64 rng(8);
    std::vector<std::string> v;
    v.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::string id = std::to_string(1'000'000 + i), qty = std::to_string(100 * (1 + rng() % 50)),
                          px = std::to_string(70 + rng() % 5) + "." + std::to_string(10 + rng() % 90);
        v.push_back("11=" + id + "\x01" "21=1\x01" "55=VOD.L\x01" "54=1\x01" "60=20261024-09:30:00.120\x01" "38=" + qty +
                    "\x01" "40=2\x01" "44=" + px + "\x01");
    }
    return v;
}

}  // namespace stage
