// Tests for include/fixgw/journal.hpp. Spec: docs/design/004-store.md (S3):
// the record format read back exactly, and a torn tail of any length cut.
#include "fix_text.hpp"

#include <fixgw/journal.hpp>

#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace fixgw;
using fixgw::test::check;

// A fresh directory on disk (the build directory, under /: /tmp is memory).
std::string temp_dir() {
    char t[] = "journal_test_XXXXXX";
    const char* d = ::mkdtemp(t);
    return d ? std::string(d) : std::string();
}

std::string read_all(const std::string& path) {
    std::string s;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return s;
    char b[4096];
    std::size_t n;
    while ((n = std::fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}
void write_all(const std::string& path, const std::string& s) {
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
}

void crc() {
    const char* v = "123456789";
    check(detail::crc32c(0, v, 9) == 0xE3069283u && detail::crc32c_table(0, reinterpret_cast<const unsigned char*>(v), 9) == 0xE3069283u,
          "CRC32C of \"123456789\" is E3069283 (the standard check value), hardware and table");
    std::mt19937 rng(7);
    std::vector<unsigned char> buf(300);
    for (auto& c : buf) c = static_cast<unsigned char>(rng());
    bool same = true;
    for (std::size_t off = 0; off < 16; ++off)
        for (std::size_t n = 0; n + off <= 280; ++n)
            same = same && detail::crc32c(0, buf.data() + off, n) == detail::crc32c_table(0, buf.data() + off, n);
    check(same, "CRC32C: the hardware path equals the table at every length 0-280 and alignment 0-15");
}

struct written {
    record_kind kind;
    session_time t;
    std::string payload;
};

std::vector<written> sample() {
    return {{record_kind::connected, {1, 1000}, ""},
            {record_kind::bytes, {2, 1001}, test::message("35=0|49=VENUE|56=CLIENT|34=1|52=20261021-07:30:00.000|")},
            {record_kind::tick, {3'000'000'000, 4000}, ""},
            {record_kind::send, {4, 4001}, std::string("\x01" "D") + test::soh("11=A|55=VOD.L|")},
            {record_kind::logout, {5, 4002}, "end of day"},
            {record_kind::disconnected, {6, 4003}, ""}};
}

bool equal(const journal_reader& r, const std::vector<written>& w, std::size_t n) {
    if (r.records().size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = r.records()[i];
        if (a.kind != w[i].kind || a.time.mono_ns != w[i].t.mono_ns || a.time.utc_ms != w[i].t.utc_ms ||
            a.payload != w[i].payload)
            return false;
    }
    return true;
}

void round_trip(const std::string& dir) {
    const std::string path = dir + "/rt.journal";
    journal_file f;
    check(f.create(path, 3), "create a journal for epoch 3");
    check(!journal_file().create(path, 3), "create refuses an existing file");
    const auto w = sample();
    bool ok = true;
    for (const auto& r : w) {
        const auto& p = r.payload;
        ok = ok && (r.kind == record_kind::send ? f.append(r.kind, r.t, p.substr(0, 2), p.substr(2)) : f.append(r.kind, r.t, p));
    }
    f.close();
    journal_reader rd;
    check(ok && rd.open(path) && rd.epoch() == 3 && !rd.torn() && equal(rd, w, w.size()),
          "every record kind read back exactly: kind, both clocks, payload; epoch kept");
    check(!f.append(record_kind::bytes, {0, 0}, std::string(journal_format::max_payload + 1, 'x')),
          "a payload above the limit is refused");
}

// S3: the file cut at every byte inside its last record, and corrupted.
void torn(const std::string& dir) {
    const std::string path = dir + "/torn.journal";
    auto w = sample();
    w.push_back({record_kind::bytes, {7, 4004}, test::message("35=D|49=VENUE|56=CLIENT|34=2|52=20261021-07:30:04.000|11=B|")});  // the last record has a payload
    {
        journal_file f;
        f.create(path, 1);
        for (const auto& r : w) f.append(r.kind, r.t, r.payload);
    }
    const std::string whole = read_all(path);
    journal_reader rd;
    rd.open(path);
    const std::size_t last_start = rd.valid_end() - journal_format::record_overhead - rd.records().back().payload.size();
    bool every = true;
    std::size_t cuts = 0;
    for (std::size_t len = last_start; len < whole.size(); ++len, ++cuts) {
        write_all(path, whole.substr(0, len));
        journal_reader r;
        every = every && r.open(path) && equal(r, w, w.size() - 1) && r.valid_end() == last_start &&
                (len == last_start) != r.torn();
    }
    check(every && cuts == whole.size() - last_start,
          "S3: cut at each of the " + std::to_string(cuts) + " lengths inside the last record: the others all kept, the torn one dropped");

    write_all(path, whole.substr(0, whole.size() - 3));
    journal_reader r2;
    r2.open(path);
    const bool cut_ok = r2.cut();
    {
        journal_file f;
        f.open_append(path, r2.valid_end());
        f.append(record_kind::tick, {8, 5000}, "");
    }
    journal_reader r3;
    r3.open(path);
    check(cut_ok && !r3.torn() && r3.records().size() == w.size() && r3.records().back().kind == record_kind::tick &&
              r3.records().back().time.mono_ns == 8,
          "S3: cut, then appended to: the new record follows the last complete one");

    std::string bad = whole;
    bad[journal_format::header_size + 30] ^= 0x20;  // inside the second record
    write_all(path, bad);
    journal_reader r4;
    r4.open(path);
    check(r4.records().size() == 1 && r4.torn(), "a corrupted record: reading stops before it");

    std::string big = whole;
    const std::uint32_t huge = 0x7FFFFFFF;
    std::memcpy(big.data() + journal_format::header_size, &huge, 4);
    write_all(path, big);
    journal_reader r5;
    r5.open(path);
    check(r5.records().empty() && r5.valid_end() == journal_format::header_size,
          "a length beyond the limit: treated as torn, nothing read past it");

    write_all(path, "NOTAJOURNAL1");
    journal_reader r6;
    check(!r6.open(path), "a wrong header: refused");
}

// D5: a pre-filled journal, written in place, extended when full; and the
// cut that zeroes records stranded after a hole.
void prefilled(const std::string& dir) {
    const std::string path = dir + "/pre.journal";
    const auto w = sample();
    {
        journal_file f;
        f.create(path, 1, 256);  // smaller than the records: extended as they come
        for (int round = 0; round < 3; ++round)
            for (const auto& r : w) f.append(r.kind, r.t, r.payload);
    }
    journal_reader r;
    r.open(path);
    bool same = r.records().size() == 3 * w.size();
    for (std::size_t i = 0; same && i < r.records().size(); ++i)
        same = r.records()[i].kind == w[i % w.size()].kind && r.records()[i].payload == w[i % w.size()].payload;
    check(same && r.file_size() % 256 == 0 && r.file_size() > 256 && r.file_size() >= r.valid_end() && !r.torn(),
          "D5: pre-filled to 256 bytes and extended in 256-byte chunks: every record read back (" +
              std::to_string(r.valid_end()) + " of " + std::to_string(r.file_size()) + " bytes)");

    // A hole in the middle (a lost write), intact records after it: the cut
    // zeroes them, and they are never read back after new appends.
    std::string bytes = read_all(path);
    const std::size_t hole = journal_format::header_size + 10;
    for (std::size_t i = hole; i < hole + 8; ++i) bytes[i] = 0;
    write_all(path, bytes);
    journal_reader r2;
    r2.open(path);
    const bool cut = r2.cut();
    {
        journal_file f;
        f.open_append(path, r2.valid_end(), 256);
        f.append(record_kind::tick, {99, 9999}, "");
    }
    journal_reader r3;
    r3.open(path);
    check(cut && r2.records().empty() && r3.records().size() == 1 && r3.records()[0].time.mono_ns == 99 && !r3.torn(),
          "D5: records after a hole are zeroed by the cut; after appending, only the new record is read");
}

}  // namespace

int main() {
    const std::string dir = temp_dir();
    if (dir.empty()) {
        check(false, "temporary directory");
        return test::result();
    }
    crc();
    round_trip(dir);
    torn(dir);
    prefilled(dir);
    std::filesystem::remove_all(dir);
    return test::result();
}
