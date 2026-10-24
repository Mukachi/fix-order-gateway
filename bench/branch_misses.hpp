// This thread's branch mispredictions, user space only (codec_bench, note 009;
// stage_bench, note 008).
#pragma once

#include <cstdint>

#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>

// Read outside the timing windows. Needs perf_event_paranoid 2 or lower (bench-on
// sets it).
class branch_misses {
public:
    branch_misses() {
        perf_event_attr a{};
        a.type = PERF_TYPE_HARDWARE;
        a.size = sizeof(a);
        a.config = PERF_COUNT_HW_BRANCH_MISSES;
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        fd_ = static_cast<int>(syscall(SYS_perf_event_open, &a, 0, -1, -1, 0));
    }
    ~branch_misses() {
        if (fd_ >= 0) close(fd_);
    }
    branch_misses(const branch_misses&) = delete;
    branch_misses& operator=(const branch_misses&) = delete;
    bool ok() const { return fd_ >= 0; }
    std::uint64_t read() const {
        std::uint64_t v = 0;
        if (::read(fd_, &v, sizeof(v)) != sizeof(v)) v = 0;
        return v;
    }

private:
    int fd_ = -1;
};
