#include "armsx5/gpu/resource.h"

namespace armsx5::gpu {

bool ResourceRegistry::overlaps_any(uint64_t addr, uint64_t size) const {
    auto it = buffers_.upper_bound(addr);
    if (it != buffers_.begin()) --it;
    for (; it != buffers_.end() && it->first < addr + size; ++it) {
        if (addr < it->first + it->second.size) return true;
    }
    for (const auto& [base, img] : images_) {
        const uint64_t approx_end = base + static_cast<uint64_t>(img.width) * img.height * 4;
        if (addr < approx_end && base < addr + size) return true;
    }
    return false;
}

std::expected<void, Error> ResourceRegistry::register_buffer(uint64_t guest_addr, uint64_t size) {
    if (size == 0)
        return std::unexpected(Error{Status::InvalidArgument, "zero-size buffer"});
    if (overlaps_any(guest_addr, size))
        return std::unexpected(Error{Status::InvalidArgument, "resource overlap"});
    buffers_[guest_addr] = BufferRecord{guest_addr, size, Residency::Resident};
    return {};
}

std::expected<void, Error> ResourceRegistry::register_image(uint64_t guest_addr, uint32_t w,
                                                            uint32_t h, uint32_t format) {
    if (w == 0 || h == 0)
        return std::unexpected(Error{Status::InvalidArgument, "zero-size image"});
    if (overlaps_any(guest_addr, static_cast<uint64_t>(w) * h * 4))
        return std::unexpected(Error{Status::InvalidArgument, "resource overlap"});
    images_[guest_addr] = ImageRecord{guest_addr, w, h, format, Residency::Resident};
    return {};
}

const BufferRecord* ResourceRegistry::find_buffer(uint64_t guest_addr) const {
    auto it = buffers_.find(guest_addr);
    return it == buffers_.end() ? nullptr : &it->second;
}

const ImageRecord* ResourceRegistry::find_image(uint64_t guest_addr) const {
    auto it = images_.find(guest_addr);
    return it == images_.end() ? nullptr : &it->second;
}

void ResourceRegistry::set_residency(uint64_t guest_addr, Residency r) {
    if (auto it = buffers_.find(guest_addr); it != buffers_.end()) it->second.residency = r;
    if (auto it = images_.find(guest_addr); it != images_.end()) it->second.residency = r;
}

size_t ResourceRegistry::evicted_count() const {
    size_t n = 0;
    for (const auto& [a, b] : buffers_)
        if (b.residency == Residency::Evicted) ++n;
    for (const auto& [a, i] : images_)
        if (i.residency == Residency::Evicted) ++n;
    return n;
}

} // namespace armsx5::gpu
