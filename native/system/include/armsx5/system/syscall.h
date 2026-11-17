#pragma once
// FreeBSD/amd64 syscall numbers and errno values relevant to the PS5 kernel
// surface. The PS5 kernel is FreeBSD 11-based but diverges from stock FreeBSD;
// numbering below follows stock FreeBSD amd64 (verified against
// freebsd-src/sys/kern/syscalls.master, see docs/research/facts.md). PS5
// specific numbers are a research item — nothing here claims PS5 accuracy
// beyond the documented FreeBSD baseline.
#include <cstdint>

namespace armsx5::system::fbsd {

// syscalls.master (amd64). Prefixed with k to avoid collisions with the
// host's SYS_* macros.
inline constexpr uint64_t kExit = 1;
inline constexpr uint64_t kFork = 2;
inline constexpr uint64_t kRead = 3;
inline constexpr uint64_t kWrite = 4;
inline constexpr uint64_t kOpen = 5;
inline constexpr uint64_t kClose = 6;
inline constexpr uint64_t kMunmap = 73;
inline constexpr uint64_t kMprotect = 74;
inline constexpr uint64_t kSchedYield = 331;
inline constexpr uint64_t kThrNew = 455;
inline constexpr uint64_t kMmap = 477;

// errno (FreeBSD values, used with the carry-flag error convention).
inline constexpr uint64_t kEPERM = 1;
inline constexpr uint64_t kENOENT = 2;
inline constexpr uint64_t kEBADF = 9;
inline constexpr uint64_t kENOMEM = 12;
inline constexpr uint64_t kEINVAL = 22;
inline constexpr uint64_t kENOSYS = 78;

// mmap protection bits (same numeric values as POSIX, renamed for the
// same macro-collision reason)
inline constexpr uint64_t kProtRead = 1;
inline constexpr uint64_t kProtWrite = 2;
inline constexpr uint64_t kProtExec = 4;

} // namespace armsx5::system::fbsd
