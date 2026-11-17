#pragma once
// GPU resource tracking: guest buffers/images known to the command processor,
// with a residency model used by later VRAM-management work.
#include "armsx5/core/result.h"

#include <expected>
#include <cstdint>
#include <map>
#include <string>

namespace armsx5::gpu {

enum class Residency : uint8_t { Resident = 0, Evicted = 1 };

struct BufferRecord {
    uint64_t guest_addr = 0;
    uint64_t size = 0;
    Residency residency = Residency::Resident;
};

struct ImageRecord {
    uint64_t guest_addr = 0;
    uint32_t width = 0, height = 0;
    uint32_t format = 0; // guest format code (VA_ENC-like; research pending)
    Residency residency = Residency::Resident;
};

class ResourceRegistry {
public:
    // Registers a guest-backed buffer. Overlapping registrations are rejected
    // so residency decisions cannot alias resources.
    std::expected<void, Error> register_buffer(uint64_t guest_addr, uint64_t size);
    std::expected<void, Error> register_image(uint64_t guest_addr, uint32_t w, uint32_t h,
                                              uint32_t format);

    const BufferRecord* find_buffer(uint64_t guest_addr) const;
    const ImageRecord* find_image(uint64_t guest_addr) const;

    void set_residency(uint64_t guest_addr, Residency r);

    size_t buffer_count() const { return buffers_.size(); }
    size_t image_count() const { return images_.size(); }
    size_t evicted_count() const;

private:
    bool overlaps_any(uint64_t addr, uint64_t size) const;
    std::map<uint64_t, BufferRecord> buffers_;
    std::map<uint64_t, ImageRecord> images_;
};

} // namespace armsx5::gpu
