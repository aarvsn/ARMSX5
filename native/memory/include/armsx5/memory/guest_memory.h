#pragma once
// Guest (x86-64) virtual address space for the ARMSX5 emulator.
//
// The guest address space is *logical*: guest addresses are translated through
// this manager into host pointers backed by anonymous host memory. The
// interpreter and JIT never dereference a guest address without a successful
// translate(), which enforces bounds and permissions (limits the blast radius
// of malformed executables).
//
// Page size is fixed at 4 KiB (matches x86-64 guest pages and Android's 4 KiB
// page size on current devices; 16 KiB-page Android support is tracked in
// docs/research/android-portability.md).
#include "armsx5/core/result.h"

#include <expected>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_set>

namespace armsx5::memory {

enum class Prot : unsigned { None = 0, R = 1, W = 2, X = 4 };

constexpr Prot operator|(Prot a, Prot b) {
    return static_cast<Prot>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr bool has(Prot p, Prot need) {
    return (static_cast<unsigned>(p) & static_cast<unsigned>(need)) == static_cast<unsigned>(need);
}

class GuestMemory {
public:
    static constexpr size_t kPageSize = 4096;

    explicit GuestMemory(uint64_t address_space_size = 0x100000000ull); // default 4 GiB
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // Maps [guest_base, guest_base+size) as guest memory backed by anonymous
    // host memory. Rejects overlaps and unaligned/zero sizes.
    std::expected<void, Error> map(uint64_t guest_base, size_t size, Prot prot);

    // Removes a mapping previously established with map() (base must match).
    std::expected<void, Error> unmap(uint64_t guest_base);

    // Updates permissions for every existing mapping intersecting [base,
    // base+size). Partial-range updates apply to intersected mappings only
    // when the range fully covers them; otherwise the call fails so that
    // permission state never silently diverges from the caller's intent.
    std::expected<void, Error> protect(uint64_t base, size_t size, Prot prot);

    // Resolves a guest address range to a host pointer. Fails on unmapped
    // addresses, ranges crossing a mapping boundary, and missing permissions.
    std::expected<void*, Error> translate(uint64_t addr, size_t size, Prot need) const;

    std::expected<void, Error> read(uint64_t addr, void* dst, size_t size) const;
    std::expected<void, Error> write(uint64_t addr, const void* src, size_t size);

    // Bump allocator inside the managed address space. Returned ranges are
    // mapped with the requested permissions.
    std::expected<uint64_t, Error> allocate(size_t size, size_t alignment, Prot prot);

    // Reserves an address range without mapping it (for callers that map
    // sub-ranges themselves, e.g. the ELF loader sliding ET_DYN segments).
    std::expected<uint64_t, Error> reserve_range(size_t size, size_t alignment);

    // Marks pages executable from the guest's point of view; the JIT uses this
    // to know which pages invalidate cached blocks when written.
    void mark_exec(uint64_t base, size_t size);
    bool is_exec_page(uint64_t addr) const;

    size_t mapping_count() const;
    uint64_t mapped_bytes() const;
    uint64_t address_space_size() const { return address_space_size_; }

private:
    // Internal: map() with mutex already held (used by allocate()).
    std::expected<void, Error> map_locked(uint64_t guest_base, size_t size, Prot prot);

    uint64_t address_space_size_;
    struct Mapping {
        uint64_t base;
        size_t size;
        Prot prot;
        uint8_t* host;
    };
    std::map<uint64_t, Mapping> mappings_; // keyed by guest base
    std::unordered_set<uint64_t> exec_pages_;
    uint64_t bump_next_ = 0x10000000ull; // bump allocator cursor
    mutable std::mutex mutex_;
};

} // namespace armsx5::memory
