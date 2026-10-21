// Tests for include/fixgw/framing.hpp. Spec: docs/design/002-codec.md (S1, S2);
// every check runs against each framer: scalar and SIMD (note 009, S1).
#include "fix_text.hpp"

#include <fixgw/framing.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace fixgw;
using fixgw::test::check;
using fixgw::test::message;

constexpr std::size_t max_frame = 4096;

using frame_fn = frame_result (*)(const char*, std::size_t, std::size_t);
frame_fn frame_under_test = nullptr;

// A reader: bytes arrive in chunks; after each, frame whatever is complete.
// Returns the frames in order, and the garbled reasons seen.
struct reader {
    std::string buffer;
    std::vector<std::string> frames, garbled;
    void feed(const std::string& chunk) {
        buffer += chunk;
        for (;;) {
            const auto r = frame_under_test(buffer.data(), buffer.size(), max_frame);
            if (r.status == frame_status::need_more) return;
            if (r.status == frame_status::complete) frames.push_back(buffer.substr(0, r.length));
            else garbled.emplace_back(r.why);
            buffer.erase(0, r.length);
        }
    }
};

void all() {
    const std::string a = message(fixgw::test::new_order_body(1));
    const std::string b = message("35=0|49=VENUE|56=CLIENT|34=7|52=20261021-09:30:01.000|");
    const std::string c = message(fixgw::test::new_order_body(2));
    const std::string stream = a + b + c;
    const std::vector<std::string> expect{a, b, c};

    {
        const auto r = frame_under_test(a.data(), a.size(), max_frame);
        check(r.status == frame_status::complete && r.length == a.size(), "a whole message is framed, its length exact");
        check(frame_under_test(a.data(), 4, max_frame).status == frame_status::need_more, "a partial prefix needs more");
    }

    // S1: every split point, byte by byte, and random coalesced reads.
    {
        bool all = true;
        for (std::size_t cut = 0; cut <= stream.size(); ++cut) {
            reader rd;
            rd.feed(stream.substr(0, cut));
            rd.feed(stream.substr(cut));
            all = all && rd.frames == expect && rd.garbled.empty() && rd.buffer.empty();
        }
        check(all, "S1 split into two reads at every point: the same three messages");
        reader one;
        for (char ch : stream) one.feed(std::string(1, ch));
        check(one.frames == expect && one.garbled.empty(), "S1 byte by byte: the same three messages");
        std::mt19937 rng(42);
        bool random_ok = true;
        for (int trial = 0; trial < 2000; ++trial) {
            reader rd;
            const std::string s = stream + stream;
            for (std::size_t i = 0; i < s.size();) {
                const std::size_t k = std::min<std::size_t>(s.size() - i, 1 + rng() % 200);
                rd.feed(s.substr(i, k));
                i += k;
            }
            random_ok = random_ok && rd.frames.size() == 6 && rd.frames[3] == a && rd.frames[5] == c && rd.garbled.empty();
        }
        check(random_ok, "S1 2,000 random splits of six coalesced messages");
    }

    // S2: each garbled fault is detected, and framing recovers at the next message.
    const auto garbled_then_recovers = [&](const std::string& bad, const char* name) {
        reader rd;
        rd.feed(bad + b);
        check(rd.garbled.size() == 1 && rd.frames.size() == 1 && rd.frames[0] == b && rd.buffer.empty(),
              std::string("S2 ") + name + ": garbled (" + (rd.garbled.empty() ? "none" : rd.garbled[0]) +
                  "), then the next message is framed");
    };
    {
        std::string v = a;
        v.replace(6, 3, "4.2");
        garbled_then_recovers(v, "FIX.4.2 BeginString");
    }
    garbled_then_recovers(fixgw::test::soh("8=FIX.4.4|9=1x2|35=0|10=000|"), "BodyLength not a number");
    garbled_then_recovers(fixgw::test::soh("8=FIX.4.4|9=|35=0|10=000|"), "BodyLength empty");
    garbled_then_recovers(message("49=X|35=0|34=1|"), "MsgType not third");
    garbled_then_recovers(message(fixgw::test::new_order_body(), 0, 1), "CheckSum wrong");
    garbled_then_recovers(message(fixgw::test::new_order_body(), -3), "BodyLength too short");
    garbled_then_recovers(message(fixgw::test::new_order_body(), +5), "BodyLength too long");
    garbled_then_recovers("garbage before the message\x01", "bytes that are not FIX");
    {
        const std::string big = message("35=D|58=" + std::string(5000, 'x') + "|");
        reader rd;
        rd.feed(big + b);
        check(!rd.garbled.empty() && rd.frames.size() == 1 && rd.frames[0] == b,
              "S2 a frame longer than the buffer is garbled; the next message is framed");
    }
    {
        // A checksum computed independently, against the framer's.
        const std::string m = fixgw::test::soh("8=FIX.4.4|9=5|35=0|");
        unsigned sum = 0;
        for (unsigned char ch : m) sum += ch;
        check(checksum_scalar(m.data(), m.size()) == sum % 256 && checksum_simd(m.data(), m.size()) == sum % 256,
              "CheckSum is the byte sum modulo 256, scalar and SIMD");
    }
}

}  // namespace

int main() {
    std::printf("== framer: scalar\n");
    frame_under_test = frame_scalar;
    all();
    std::printf("== framer: SIMD\n");
    frame_under_test = frame_simd;
    all();
    // CheckSum at every length up to 300 and every alignment: SIMD equals scalar.
    std::string bytes(400, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i * 131 + 7);
    bool same = true;
    for (std::size_t off = 0; off < 32; ++off)
        for (std::size_t n = 0; n + off <= 300; ++n)
            same = same && checksum_simd(bytes.data() + off, n) == checksum_scalar(bytes.data() + off, n);
    check(same, "S1 SIMD CheckSum equals scalar at every length 0-300 and every alignment");
    return fixgw::test::result();
}
