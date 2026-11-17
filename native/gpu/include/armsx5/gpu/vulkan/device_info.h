#pragma once
// Vulkan runtime introspection via dlopen("libvulkan.so[.1]").
//
// The Vulkan *loader* is a runtime dependency of the Android OS (API 24+)
// and of desktop hosts; ARMSX5 deliberately does not link against it, so
// the core builds and tests run on machines without any Vulkan ICD.
// Device selection is capability-based (API version, extensions, driver)
// and never assumes identical behavior across GPU vendors
// (docs/architecture/gpu.md).
#include "armsx5/core/result.h"

#include <expected>
#include <cstdint>
#include <string>
#include <vector>

namespace armsx5::gpu::vulkan {

struct PhysicalDeviceInfo {
    std::string device_name;
    uint32_t api_version = 0;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t driver_version = 0;
    std::string driver_name;   // VK_KHR_driver_properties when available
    std::vector<std::string> device_extensions;
};

struct RuntimeInfo {
    uint32_t instance_api_version = 0;
    uint32_t physical_device_count = 0;
    std::vector<PhysicalDeviceInfo> devices;
    std::string to_json() const;
};

// Enumerates the Vulkan runtime. Returns VulkanError with a human-readable
// reason when no loader/ICD is present (this is an expected state on
// development machines, not a crash).
std::expected<RuntimeInfo, Error> probe_runtime();

} // namespace armsx5::gpu::vulkan
