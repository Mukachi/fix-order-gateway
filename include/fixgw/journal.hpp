// The session journal (note 004): an append-only file of the session's
// inputs, each record checksummed, read back for recovery with a torn tail
// detected and cut. Design: docs/design/004-store.md
//
// A file starts with a header, then records:
//   header: "FIXGWJ1" NUL, epoch (u32): the sequence epoch, 1 for a new
//           session, one more at each sequence reset (a new file each time)
//   record: length (u32, payload bytes), kind (u8), mono_ns (i64),
//           utc_ms (i64), payload, CRC32C (u32) of everything before it in
//           the record
// Integers little-endian (x86-64 and aarch64 both are; a big-endian build
// is refused).
#pragma once

#include <fixgw/session.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#elif defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the journal format is little-endian");

namespace fixgw {

namespace journal_format {
inline constexpr std::string_view magic{"FIXGWJ1\0", 8};
inline constexpr std::size_t header_size = magic.size() + 4;
inline constexpr std::size_t record_overhead = 4 + 1 + 8 + 8 + 4;  // length, kind, clocks, CRC
inline constexpr std::size_t max_payload = 1 << 20;                  // a record larger than this is corrupt
}  // namespace journal_format

// What a record holds: one input of the session (note 004, Design).
enum class record_kind : std::uint8_t { connected = 1, disconnected = 2, bytes = 3, tick = 4, send = 5, logout = 6 };

namespace detail {

// CRC32C (Castagnoli), reflected, as iSCSI and ext4 use it; the table version
// is the reference the hardware paths are tested against.
inline std::uint32_t crc32c_table(std::uint32_t crc, const unsigned char* p, std::size_t n) noexcept {
    static constexpr auto table = [] {
        struct t {
            std::uint32_t v[256];
        } r{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? (c >> 1) ^ 0x82F63B78u : c >> 1;
            r.v[i] = c;
        }
        return r;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table.v[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline std::uint32_t crc32c(std::uint32_t crc, const void* data, std::size_t n) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
#if defined(__SSE4_2__)
    std::uint64_t c = ~crc;
    for (; n >= 8; n -= 8, p += 8) {
        std::uint64_t v;
        std::memcpy(&v, p, 8);
        c = _mm_crc32_u64(c, v);
    }
    std::uint32_t c32 = static_cast<std::uint32_t>(c);
    for (; n > 0; --n, ++p) c32 = _mm_crc32_u8(c32, *p);
    return ~c32;
#elif defined(__ARM_FEATURE_CRC32)
    std::uint32_t c = ~crc;
    for (; n >= 8; n -= 8, p += 8) {
        std::uint64_t v;
        std::memcpy(&v, p, 8);
        c = __crc32cd(c, v);
    }
    for (; n > 0; --n, ++p) c = __crc32cb(c, *p);
    return ~c;
#else
    return crc32c_table(crc, p, n);
#endif
}

template <class T>
inline void put(char*& at, T v) noexcept {
    std::memcpy(at, &v, sizeof(v));
    at += sizeof(v);
}
template <class T>
inline T get(const char* at) noexcept {
    T v;
    std::memcpy(&v, at, sizeof(v));
    return v;
}

}  // namespace detail

// Encodes one record into `out` (record_overhead + payload bytes); the payload
// is `a` followed by `b` (a send record: a length byte and the MsgType, then
// the body). Returns its size.
inline std::size_t encode_record(char* out, record_kind kind, session_time t, std::string_view a,
                                 std::string_view b = {}) noexcept {
    char* at = out;
    detail::put(at, static_cast<std::uint32_t>(a.size() + b.size()));
    detail::put(at, static_cast<std::uint8_t>(kind));
    detail::put(at, t.mono_ns);
    detail::put(at, t.utc_ms);
    if (!a.empty()) std::memcpy(at, a.data(), a.size());  // an empty view's data() may be null
    at += a.size();
    if (!b.empty()) std::memcpy(at, b.data(), b.size());
    at += b.size();
    detail::put(at, detail::crc32c(0, out, static_cast<std::size_t>(at - out)));
    return static_cast<std::size_t>(at - out);
}

struct journal_record {
    record_kind kind;
    session_time time;
    std::string_view payload;  // a view into the reader's copy of the file
};

// Reads a whole journal file (recovery is not on the hot path): its epoch,
// every complete record whose CRC holds, and where those end. Reading stops
// at the first record that is short, too long or fails its CRC: a torn write
// from a crash, never released (note 004, D1), so cut.
class journal_reader {
public:
    // False if the file cannot be read or its header is wrong.
    bool open(const std::string& path) {
        path_ = path;
        bytes_.clear();
        records_.clear();
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        char buf[1 << 16];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            bytes_.insert(bytes_.end(), buf, buf + n);
        }
        ::close(fd);
        if (bytes_.size() < journal_format::header_size ||
            std::string_view(bytes_.data(), journal_format::magic.size()) != journal_format::magic)
            return false;
        epoch_ = detail::get<std::uint32_t>(bytes_.data() + journal_format::magic.size());
        std::size_t at = journal_format::header_size;
        for (;;) {
            if (bytes_.size() - at < journal_format::record_overhead) break;
            const std::uint32_t len = detail::get<std::uint32_t>(bytes_.data() + at);
            if (len > journal_format::max_payload || bytes_.size() - at < journal_format::record_overhead + len) break;
            const std::size_t body = journal_format::record_overhead - 4 + len;
            const std::uint32_t crc = detail::get<std::uint32_t>(bytes_.data() + at + body);
            if (crc != detail::crc32c(0, bytes_.data() + at, body)) break;
            const char* r = bytes_.data() + at;
            records_.push_back({static_cast<record_kind>(detail::get<std::uint8_t>(r + 4)),
                                {detail::get<std::int64_t>(r + 5), detail::get<std::int64_t>(r + 13)},
                                {r + 21, len}});
            at += body + 4;
        }
        valid_end_ = at;
        return true;
    }
    std::uint32_t epoch() const noexcept { return epoch_; }
    const std::vector<journal_record>& records() const noexcept { return records_; }
    std::size_t valid_end() const noexcept { return valid_end_; }
    std::size_t file_size() const noexcept { return bytes_.size(); }
    // Whether anything but zeros follows the last complete record (a torn
    // write, or records stranded after a hole); a pre-filled file's zero rest
    // is not torn (D5).
    bool torn() const noexcept {
        for (std::size_t i = valid_end_; i < bytes_.size(); ++i)
            if (bytes_[i] != 0) return true;
        return false;
    }

    // Cuts the journal back to its last complete record: zeroes everything
    // from there to the last non-zero byte, never truncating (a pre-filled
    // file keeps its size, D5). Records after a hole (group commit's writes
    // that outlived an earlier, lost one) are zeroed too, so none can be read
    // back once appending resumes at `valid_end`. False on failure.
    bool cut() const {
        std::size_t last = bytes_.size();
        while (last > valid_end_ && bytes_[last - 1] == 0) --last;
        if (last == valid_end_) return true;
        const int fd = ::open(path_.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const std::vector<char> zeros(last - valid_end_, 0);
        const bool ok = ::pwrite(fd, zeros.data(), zeros.size(), static_cast<off_t>(valid_end_)) ==
                            static_cast<ssize_t>(zeros.size()) &&
                        ::fdatasync(fd) == 0;
        ::close(fd);
        return ok;
    }

private:
    std::string path_;
    std::vector<char> bytes_;
    std::vector<journal_record> records_;
    std::uint32_t epoch_ = 0;
    std::size_t valid_end_ = 0;
};

// Writes records to a journal file with pwrite(2), each after the last.
// Durability (when an appended record counts as safe) is the strategies'
// (note 004, A to D), layered on this. Pre-filled (D5): created zero-filled
// to `prefill` bytes and written in place, so an fdatasync has only data to
// write, no change of size to commit; extended by another `prefill` of zeros
// (written and synced) when a record would pass the end. The record buffer is
// allocated once, at open.
class journal_file {
public:
    journal_file() = default;
    journal_file(const journal_file&) = delete;
    journal_file& operator=(const journal_file&) = delete;
    ~journal_file() { close(); }

    // Creates a new file for `epoch`, failing if it exists; `prefill` 0
    // means no pre-fill (the file grows with each record). Built under a
    // temporary name (header, pre-fill, synced) and renamed into place, the
    // directory synced: a crash leaves no journal or a whole one, never one
    // without its header.
    bool create(const std::string& path, std::uint32_t epoch, std::size_t prefill = 0) {
        close();
        if (::access(path.c_str(), F_OK) == 0) return false;
        const std::string tmp = path + ".new";
        ::unlink(tmp.c_str());  // a half-built one from an earlier crash
        fd_ = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd_ < 0) return false;
        buffer_.resize(journal_format::record_overhead + journal_format::max_payload);
        char h[journal_format::header_size];
        std::memcpy(h, journal_format::magic.data(), journal_format::magic.size());
        std::memcpy(h + journal_format::magic.size(), &epoch, 4);
        at_ = 0;
        if (!write_at(h, sizeof(h))) return false;
        size_ = at_;
        chunk_ = prefill;
        if (prefill > size_) {  // the first chunk: up to `prefill`, after the header
            const std::size_t keep = chunk_;
            chunk_ = prefill - size_;
            const bool ok = extend();
            chunk_ = keep;
            if (!ok) return false;
        } else if (::fdatasync(fd_) != 0) {
            return false;
        }
        if (::rename(tmp.c_str(), path.c_str()) != 0) return false;
        const std::string dir = path.find('/') == std::string::npos ? "." : path.substr(0, path.rfind('/'));
        const int d = ::open(dir.empty() ? "/" : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const bool synced = d >= 0 && ::fsync(d) == 0;
        if (d >= 0) ::close(d);
        return synced;
    }
    // Opens an existing journal to continue after its last complete record
    // (`valid_end`, from the reader, after `cut()`), extending by `prefill`
    // when full (0: the file grows with each record).
    bool open_append(const std::string& path, std::size_t valid_end, std::size_t prefill = 0) {
        close();
        fd_ = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd_ < 0) return false;
        buffer_.resize(journal_format::record_overhead + journal_format::max_payload);
        const off_t size = ::lseek(fd_, 0, SEEK_END);
        size_ = size > 0 ? static_cast<std::size_t>(size) : 0;
        at_ = valid_end;
        chunk_ = prefill;
        return true;
    }
    // Where the next record goes: the end of the records written.
    std::size_t end() const noexcept { return at_; }
    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    int fd() const noexcept { return fd_; }

    // Appends one record; false if it is too large or a write fails.
    bool append(record_kind kind, session_time t, std::string_view a, std::string_view b = {}) noexcept {
        if (a.size() + b.size() > journal_format::max_payload) return false;
        last_ = encode_record(buffer_.data(), kind, t, a, b);
        while (chunk_ > 0 && at_ + last_ > size_)
            if (!extend()) return false;
        return write_at(buffer_.data(), last_);
    }
    // The bytes of the record last appended (for a strategy that also sends it).
    std::string_view last_record() const noexcept { return {buffer_.data(), last_}; }

private:
    bool write_at(const char* p, std::size_t n) noexcept {
        while (n > 0) {
            const ssize_t w = ::pwrite(fd_, p, n, static_cast<off_t>(at_));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return false;
            p += w;
            at_ += static_cast<std::size_t>(w);
            n -= static_cast<std::size_t>(w);
        }
        return true;
    }
    // Another chunk of zeros past the end, written and synced (D5).
    bool extend() noexcept {
        static const char zeros[1 << 16] = {};  // not buffer_: it may hold the record being appended
        std::size_t left = chunk_;
        while (left > 0) {
            const std::size_t n = left < sizeof(zeros) ? left : sizeof(zeros);
            const ssize_t w = ::pwrite(fd_, zeros, n, static_cast<off_t>(size_));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return false;
            size_ += static_cast<std::size_t>(w);
            left -= static_cast<std::size_t>(w);
        }
        return ::fdatasync(fd_) == 0;
    }
    int fd_ = -1;
    std::vector<char> buffer_;
    std::size_t last_ = 0, at_ = 0, size_ = 0, chunk_ = 0;
};

}  // namespace fixgw
