#include "armsx5/gpu/vulkan/device_info.h"

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <cstring>
#include <string>
#include <vector>

namespace armsx5::gpu::vulkan {
namespace {

std::string version_string(uint32_t v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
             VK_API_VERSION_PATCH(v));
    return buf;
}

} // namespace

std::string RuntimeInfo::to_json() const {
    std::string out = "{\"instance_api_version\":\"" + version_string(instance_api_version) +
                      "\",\"device_count\":" + std::to_string(physical_device_count) +
                      ",\"devices\":[";
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& d = devices[i];
        if (i) out += ",";
        out += "{\"name\":\"" + d.device_name + "\",\"api\":\"" + version_string(d.api_version) +
               "\",\"vendor_id\":" + std::to_string(d.vendor_id) +
               ",\"device_id\":" + std::to_string(d.device_id) +
               "\",\"driver\":\"" + version_string(d.driver_version) +
               "\",\"extensions\":" + std::to_string(d.device_extensions.size()) + "}";
    }
    out += "]}";
    return out;
}

std::expected<RuntimeInfo, Error> probe_runtime() {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return std::unexpected(Error{Status::VulkanError,
                                     "no Vulkan loader found (dlopen libvulkan.so failed)"});

    auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(lib, "vkGetInstanceProcAddr"));
    if (!get_proc)
        return std::unexpected(Error{Status::VulkanError, "vkGetInstanceProcAddr missing"});

    RuntimeInfo info;

    auto pEnumInstanceVersion =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(get_proc(nullptr,
                                                                 "vkEnumerateInstanceVersion"));
    if (pEnumInstanceVersion) pEnumInstanceVersion(&info.instance_api_version);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ARMSX5";
    app.applicationVersion = 1;
    app.pEngineName = "ARMSX5";
    app.engineVersion = 1;
    app.apiVersion = info.instance_api_version ? info.instance_api_version
                                               : VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    auto pCreateInstance =
        reinterpret_cast<PFN_vkCreateInstance>(get_proc(nullptr, "vkCreateInstance"));
    if (!pCreateInstance)
        return std::unexpected(Error{Status::VulkanError, "vkCreateInstance missing"});
    const VkResult vr = pCreateInstance(&ci, nullptr, &instance);
    if (vr != VK_SUCCESS || instance == VK_NULL_HANDLE)
        return std::unexpected(
            Error{Status::VulkanError, "vkCreateInstance failed (vr=" + std::to_string(vr) + ")"});

    auto pEnumPhysicalDevices =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_proc(instance,
                                                                  "vkEnumeratePhysicalDevices"));
    auto pGetProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        get_proc(instance, "vkGetPhysicalDeviceProperties"));
    auto pEnumDevExt = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        get_proc(instance, "vkEnumerateDeviceExtensionProperties"));
    auto pDestroy =
        reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"));

    if (pEnumPhysicalDevices && pGetProps) {
        uint32_t n = 0;
        if (pEnumPhysicalDevices(instance, &n, nullptr) == VK_SUCCESS && n > 0) {
            info.physical_device_count = n;
            std::vector<VkPhysicalDevice> pd(n);
            pEnumPhysicalDevices(instance, &n, pd.data());
            for (uint32_t i = 0; i < n; ++i) {
                PhysicalDeviceInfo d;
                VkPhysicalDeviceProperties props{};
                pGetProps(pd[i], &props);
                d.api_version = props.apiVersion;
                d.driver_version = props.driverVersion;
                d.vendor_id = props.vendorID;
                d.device_id = props.deviceID;
                d.device_name = props.deviceName;

                if (pEnumDevExt) {
                    uint32_t ext_n = 0;
                    if (pEnumDevExt(pd[i], nullptr, &ext_n, nullptr) == VK_SUCCESS && ext_n > 0) {
                        std::vector<VkExtensionProperties> exts(ext_n);
                        pEnumDevExt(pd[i], nullptr, &ext_n, exts.data());
                        for (const auto& e : exts)
                            d.device_extensions.emplace_back(e.extensionName);
                    }
                }
                info.devices.push_back(std::move(d));
            }
        }
    }
    if (pDestroy) pDestroy(instance, nullptr);
    dlclose(lib);
    return info;
}

} // namespace armsx5::gpu::vulkan
