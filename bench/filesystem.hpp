// The file system a benchmark's journal directory is on, for its run file.
#pragma once

#include <string>

#include <sys/statfs.h>

namespace bench {

// Journal C's directory (note 004's amendment by note 008): tmpfs, where the
// kernel never writes the mapped file back (on ext4 writeback write-protects
// the page being appended to, and the next write blocks in the kernel).
inline constexpr const char* journal_c_template = "/dev/shm/fixgw_journal_c_XXXXXX";

inline std::string filesystem_of(const std::string& dir) {
    struct statfs s;
    if (::statfs(dir.c_str(), &s) != 0) return "unknown";
    switch (static_cast<unsigned long>(s.f_type)) {
        case 0xEF53: return "ext4";
        case 0x01021994: return "tmpfs";
        case 0x58465342: return "xfs";
        case 0x9123683E: return "btrfs";
        default: return "other";
    }
}

}  // namespace bench
