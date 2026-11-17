#include "armsx5/platform/exec_arena.h"

#include "armsx5/core/log.h"

#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <sys/syscall.h>
#include <linux/memfd.h>
#endif

namespace armsx5::platform {
namespace {
constexpr const char* TAG = "ExecArena";

size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }
} // namespace

ExecArena::ExecArena(size_t initial_bytes) : initial_bytes_(initial_bytes) {}

ExecArena::~ExecArena() {
    if (rw_view_ && rw_view_ != exec_view_) munmap(rw_view_, capacity_bytes_);
    if (exec_view_) munmap(exec_view_, capacity_bytes_);
    if (fd_ >= 0) close(fd_);
}

std::expected<void, Error> ExecArena::init() {
    if (capacity_bytes_ != 0) return {};
    if (!grow(align_up(initial_bytes_, static_cast<size_t>(sysconf(_SC_PAGESIZE)))))
        return std::unexpected(Error{Status::MemoryError, "cannot allocate JIT arena"});
    return {};
}

bool ExecArena::grow(size_t new_capacity) {
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    new_capacity = align_up(new_capacity, page);

#if defined(__ANDROID__)
    if (fd_ < 0) {
        fd_ = static_cast<int>(syscall(SYS_memfd_create, "armsx5-jit", MFD_CLOEXEC));
        if (fd_ < 0) {
            ARMSX5_LOG_ERROR(TAG, "memfd_create failed: %s", std::strerror(errno));
            return false;
        }
    }
    if (ftruncate(fd_, static_cast<off_t>(new_capacity)) != 0) {
        ARMSX5_LOG_ERROR(TAG, "ftruncate failed: %s", std::strerror(errno));
        return false;
    }
    // RW view for writing...
    void* rw = mmap(nullptr, new_capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (rw == MAP_FAILED) {
        ARMSX5_LOG_ERROR(TAG, "rw mmap failed: %s", std::strerror(errno));
        return false;
    }
    // ...and a separate RX view for execution (never writable).
    void* ex = mmap(nullptr, new_capacity, PROT_READ | PROT_EXEC, MAP_SHARED, fd_, 0);
    if (ex == MAP_FAILED) {
        munmap(rw, new_capacity);
        ARMSX5_LOG_ERROR(TAG, "rx mmap failed: %s", std::strerror(errno));
        return false;
    }
    if (rw_view_) munmap(rw_view_, capacity_bytes_);
    if (exec_view_ && exec_view_ != rw_view_) munmap(exec_view_, capacity_bytes_);
    rw_view_ = static_cast<uint8_t*>(rw);
    exec_view_ = static_cast<uint8_t*>(ex);
#else
    void* p = mmap(nullptr, new_capacity, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        ARMSX5_LOG_ERROR(TAG, "rwx mmap failed: %s", std::strerror(errno));
        return false;
    }
    if (rw_view_) munmap(rw_view_, capacity_bytes_);
    rw_view_ = static_cast<uint8_t*>(p);
    exec_view_ = rw_view_; // same view on desktop development hosts
#endif

    capacity_bytes_ = new_capacity;
    return true;
}

std::expected<uint8_t*, Error> ExecArena::reserve(size_t bytes) {
    if (capacity_bytes_ == 0) {
        auto r = init();
        if (!r) return std::unexpected(r.error());
    }
    if (used_bytes_ + bytes > capacity_bytes_) {
        if (!grow((used_bytes_ + bytes) * 2))
            return std::unexpected(Error{Status::MemoryError, "cannot grow JIT arena"});
    }
    uint8_t* out = rw_view_ + used_bytes_;
    used_bytes_ += bytes;
    return out;
}

void ExecArena::commit(size_t bytes) { (void)bytes; /* used_bytes_ advanced in reserve() */ }

} // namespace armsx5::platform
