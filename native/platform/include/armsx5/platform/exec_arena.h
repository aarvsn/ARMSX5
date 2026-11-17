#pragma once
// Executable-memory arena for the JIT.
//
// Android 10+ forbids anonymous mappings that are simultaneously writable and
// executable (W^X). The Android path uses the memfd_create + dual-mapping
// technique: one RW view for writing, one RX view for execution; the RX view
// is never mapped writable. On desktop development hosts a plain RWX mapping
// is used (documented in docs/architecture/cpu.md).
#include "armsx5/core/result.h"

#include <expected>
#include <cstddef>
#include <cstdint>

namespace armsx5::platform {

class ExecArena {
public:
    explicit ExecArena(size_t initial_bytes = 1u << 20);
    ~ExecArena();
    ExecArena(const ExecArena&) = delete;
    ExecArena& operator=(const ExecArena&) = delete;

    std::expected<void, Error> init();
    // Returns the write view (RW on all platforms). Grows the arena if needed.
    std::expected<uint8_t*, Error> reserve(size_t bytes);
    void commit(size_t bytes); // bytes actually used by the caller

    const uint8_t* exec_ptr() const { return exec_view_; } // RX view (or RWX on desktop)
    size_t size() const { return used_bytes_; }
    size_t capacity() const { return capacity_bytes_; }

private:
    bool grow(size_t new_capacity);
    size_t initial_bytes_;
    uint8_t* rw_view_ = nullptr;
    uint8_t* exec_view_ = nullptr;
    int fd_ = -1; // memfd on Android, -1 otherwise
    size_t used_bytes_ = 0;
    size_t capacity_bytes_ = 0;
};

} // namespace armsx5::platform
