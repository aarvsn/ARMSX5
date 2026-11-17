#include "armsx5/memory/guest_memory.h"

#include "armsx5/core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace armsx5::memory {
namespace {
constexpr const char* TAG = "GuestMemory";

size_t round_up_page(size_t v) {
    return (v + GuestMemory::kPageSize - 1) / GuestMemory::kPageSize * GuestMemory::kPageSize;
}
} // namespace

GuestMemory::GuestMemory(uint64_t address_space_size) : address_space_size_(address_space_size) {}

GuestMemory::~GuestMemory() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [base, m] : mappings_) {
        if (m.host) munmap(m.host, round_up_page(m.size));
    }
}

std::expected<void, Error> GuestMemory::map(uint64_t guest_base, size_t size, Prot prot) {
    if (size == 0) return std::unexpected(Error{Status::InvalidArgument, "zero-size map"});
    if (guest_base % kPageSize != 0)
        return std::unexpected(Error{Status::InvalidArgument, "map base not page-aligned"});
    if (guest_base + size > address_space_size_)
        return std::unexpected(Error{Status::InvalidArgument, "map range outside guest address space"});

    std::lock_guard<std::mutex> lock(mutex_);
    return map_locked(guest_base, size, prot);
}

std::expected<void, Error> GuestMemory::map_locked(uint64_t guest_base, size_t size,
                                                   Prot prot) {
    const uint64_t end = guest_base + size;
    auto it = mappings_.upper_bound(guest_base);
    if (it != mappings_.begin()) --it;
    for (; it != mappings_.end() && it->second.base < end; ++it) {
        if (guest_base < it->second.base + it->second.size) {
            return std::unexpected(Error{Status::InvalidArgument, "map range overlaps existing mapping"});
        }
    }

    const size_t rounded = round_up_page(size);
    void* host = mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (host == MAP_FAILED) {
        return std::unexpected(
            Error{Status::MemoryError, std::string("host mmap failed: ") + std::strerror(errno)});
    }
    mappings_[guest_base] = Mapping{guest_base, size, prot, static_cast<uint8_t*>(host)};
    return {};
}

std::expected<void, Error> GuestMemory::unmap(uint64_t guest_base) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = mappings_.find(guest_base);
    if (it == mappings_.end())
        return std::unexpected(Error{Status::NotFound, "no mapping at requested base"});
    munmap(it->second.host, round_up_page(it->second.size));
    for (uint64_t p = guest_base; p < guest_base + it->second.size; p += kPageSize)
        exec_pages_.erase(p / kPageSize);
    mappings_.erase(it);
    return {};
}

std::expected<void, Error> GuestMemory::protect(uint64_t base, size_t size, Prot prot) {
    if (size == 0) return std::unexpected(Error{Status::InvalidArgument, "zero-size protect"});
    std::lock_guard<std::mutex> lock(mutex_);
    bool any = false;
    auto it = mappings_.upper_bound(base);
    if (it != mappings_.begin()) --it;
    for (; it != mappings_.end() && it->second.base < base + size; ++it) {
        Mapping& m = it->second;
        const uint64_t lo = std::max(base, m.base);
        const uint64_t hi = std::min(base + size, m.base + m.size);
        if (lo >= hi) continue;
        any = true;
        // Only full-cover updates are applied; partial updates are rejected
        // below so logical permission state stays exact.
        if (lo != m.base || hi != m.base + m.size) {
            return std::unexpected(
                Error{Status::InvalidArgument, "protect range only partially covers a mapping"});
        }
        m.prot = prot;
    }
    if (!any)
        return std::unexpected(Error{Status::NotFound, "protect range not backed by any mapping"});
    return {};
}

std::expected<void*, Error> GuestMemory::translate(uint64_t addr, size_t size, Prot need) const {
    if (size == 0) return std::unexpected(Error{Status::InvalidArgument, "zero-size translate"});
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = mappings_.upper_bound(addr);
    if (it == mappings_.begin())
        return std::unexpected(Error{Status::GuestFault, "address below any mapping"});
    --it;
    const Mapping& m = it->second;
    if (addr < m.base || addr >= m.base + m.size)
        return std::unexpected(Error{Status::GuestFault, "unmapped address"});
    if (addr + size > m.base + m.size)
        return std::unexpected(Error{Status::GuestFault, "access crosses mapping boundary"});
    if (!has(m.prot, need))
        return std::unexpected(Error{Status::GuestFault, "permission denied for access"});
    return m.host + (addr - m.base);
}

std::expected<void, Error> GuestMemory::read(uint64_t addr, void* dst, size_t size) const {
    auto ptr = translate(addr, size, Prot::R);
    if (!ptr) return std::unexpected(ptr.error());
    std::memcpy(dst, *ptr, size);
    return {};
}

std::expected<void, Error> GuestMemory::write(uint64_t addr, const void* src, size_t size) {
    auto ptr = translate(addr, size, Prot::W);
    if (!ptr) return std::unexpected(ptr.error());
    std::memcpy(*ptr, src, size);
    return {};
}

std::expected<uint64_t, Error> GuestMemory::allocate(size_t size, size_t alignment, Prot prot) {
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
        return std::unexpected(Error{Status::InvalidArgument, "bad allocation size/alignment"});
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t base = (bump_next_ + alignment - 1) / alignment * alignment;
    if (base + size > address_space_size_)
        return std::unexpected(Error{Status::MemoryError, "guest address space exhausted"});
    bump_next_ = base + size;
    auto r = map_locked(base, size, prot);
    if (!r) return std::unexpected(r.error());
    return base;
}

std::expected<uint64_t, Error> GuestMemory::reserve_range(size_t size, size_t alignment) {
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
        return std::unexpected(Error{Status::InvalidArgument, "bad reservation size/alignment"});
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t base = (bump_next_ + alignment - 1) / alignment * alignment;
    if (base + size > address_space_size_)
        return std::unexpected(Error{Status::MemoryError, "guest address space exhausted"});
    bump_next_ = base + size;
    return base;
}

void GuestMemory::mark_exec(uint64_t base, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t first = base / kPageSize;
    const uint64_t last = (base + size + kPageSize - 1) / kPageSize;
    for (uint64_t p = first; p < last; ++p) exec_pages_.insert(p);
}

bool GuestMemory::is_exec_page(uint64_t addr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return exec_pages_.count(addr / kPageSize) != 0;
}

size_t GuestMemory::mapping_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mappings_.size();
}

uint64_t GuestMemory::mapped_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = 0;
    for (const auto& [base, m] : mappings_) total += m.size;
    return total;
}

} // namespace armsx5::memory
