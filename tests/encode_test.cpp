// Tests for include/fixgw/encode.hpp. Spec: docs/design/002-codec.md (S5)
#include "fix_text.hpp"

#include <fixgw/decode.hpp>
#include <fixgw/encode.hpp>

#include <cstdlib>
#include <ctime>
#include <random>
#include <string>

namespace {

using namespace fixgw;
using fixgw::test::check;

// True if `m` frames as one complete message (BodyLength and CheckSum right)
// and decodes.
bool valid(std::string_view m, message_view<>& v) {
    const auto f = frame(m.data(), m.size(), 4096);
    return f.status == frame_status::complete && f.length == m.size() && decode(m.data(), m.size(), v).ok;
}

// An independent CheckSum, from the bytes before "10=".
bool checksum_matches(std::string_view m) {
    unsigned sum = 0;
    for (std::size_t i = 0; i + trailer_length < m.size(); ++i) sum += static_cast<unsigned char>(m[i]);
    return std::stoi(std::string(m.substr(m.size() - 4, 3))) == static_cast<int>(sum % 256);
}

std::string timestamp(std::int64_t ms) {
    char out[utc_timestamp_length];
    format_utc_timestamp(ms, out);
    return std::string(out, sizeof(out));
}

std::string reference_timestamp(std::int64_t ms) {
    const std::time_t s = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&s, &tm);
    char out[96];  // room for any int, to satisfy -Wformat-truncation
    std::snprintf(out, sizeof(out), "%04d%02d%02d-%02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
    return out;
}

}  // namespace

int main() {
    // B: the same bytes as the test's own framing.
    {
        char buf[512];
        encoder e(buf, sizeof(buf));
        e.begin("D");
        e.add(49, "CLIENT");
        e.add(56, "VENUE");
        e.add(34, std::uint64_t{7});
        e.add(52, "20261021-09:30:00.123");
        e.add(11, "ORD-000001");
        e.add(1, "ACC1");
        e.add(55, "VOD.L");
        e.add(54, '1');
        e.add(60, "20261021-09:30:00.123");
        e.add(38, std::uint64_t{1000});
        e.add(40, '2');
        e.add_decimal(44, 7253, 2);
        e.add(59, '0');
        const std::string_view m = e.finish();
        check(m == fixgw::test::message(fixgw::test::new_order_body(7)),
              "S5 encoder B writes the exact bytes of an independently framed NewOrderSingle");
        message_view<> v;
        check(valid(m, v) && v.get(11) == "ORD-000001" && v.get(44) == "72.53" && checksum_matches(m),
              "S5 encode then frame and decode: valid, fields back");
    }
    {
        char buf[48];
        encoder e(buf, sizeof(buf));
        e.begin("D");
        e.add(58, std::string_view("a long text that does not fit the buffer at all"));
        check(e.finish().empty() && e.overflow(), "a message too large for the buffer: overflow, nothing returned");
    }
    {
        const auto dec = [](std::int64_t m, int d) {
            char buf[128];
            encoder e(buf, sizeof(buf));
            e.begin("0");
            e.add_decimal(44, m, d);
            message_view<> v;
            const std::string_view s = e.finish();
            return valid(s, v) ? std::string(*v.get(44)) : std::string("invalid");
        };
        check(dec(7253, 2) == "72.53" && dec(5, 2) == "0.05" && dec(-125, 1) == "-12.5" && dec(100, 0) == "100",
              "decimals: 72.53, 0.05, -12.5, 100");
    }

    // UTCTimestamp against gmtime_r: edges and a million random instants.
    {
        bool ok = true;
        for (const std::int64_t ms : {std::int64_t{0}, std::int64_t{951'782'400'000}, std::int64_t{1'709'164'800'999},
                                      std::int64_t{4'107'542'400'000}, std::int64_t{1'798'761'599'999}})
            ok = ok && timestamp(ms) == reference_timestamp(ms);
        std::mt19937_64 rng(7);
        for (int i = 0; i < 1'000'000; ++i) {
            const auto ms = static_cast<std::int64_t>(rng() % 7'258'118'400'000ULL);  // 1970-2200
            ok = ok && timestamp(ms) == reference_timestamp(ms);
        }
        check(ok, "UTCTimestamp agrees with gmtime_r: 1970, leap days 2000 and 2024, 2100, a year end, 1M random");
    }

    // C: pre-encoded template.
    {
        new_order_template t({"CLIENT", "VENUE", "ACC1", "VOD.L", '1', '2', '0', 2});
        check(t.ok(), "a NewOrderSingle template is laid out");
        std::mt19937_64 rng(11);
        bool all = true;
        std::size_t length = 0;
        for (int i = 0; i < 100'000; ++i) {
            const std::uint64_t seq = rng() % 1'000'000'000, id = rng() % 10'000'000'000'000'000ULL;
            const std::uint64_t qty = rng() % 1'000'000'000, price = rng() % 10'000'000'000ULL;
            const auto ms = static_cast<std::int64_t>(rng() % 4'102'444'800'000ULL);
            const std::string_view m = t.fill(seq, ms, id, qty, price);
            message_view<> v;
            const bool ok = valid(m, v) && checksum_matches(m) && std::stoull(std::string(*v.get(34))) == seq &&
                            std::stoull(std::string(*v.get(11))) == id && std::stoull(std::string(*v.get(38))) == qty &&
                            std::llround(std::stod(std::string(*v.get(44))) * 100) == static_cast<long long>(price) &&
                            *v.get(52) == timestamp(ms) && *v.get(60) == timestamp(ms) && v.msg_type() == "D";
            all = all && ok;
            if (i == 0) length = m.size();
            all = all && m.size() == length;
        }
        check(all, "S5 C: 100,000 fills, each valid (BodyLength, CheckSum checked independently), fields back, length constant");
        check(t.fill(1'000'000'000, 0, 1, 1, 1).empty() && t.fill(1, 0, 1, 1, 100'000'000'000ULL).empty(),
              "C: a value too wide for its slot is refused, not truncated");
    }
    return fixgw::test::result();
}
