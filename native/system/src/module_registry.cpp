#include "armsx5/system/module_registry.h"

#include "armsx5/core/log.h"

namespace armsx5::system {
namespace {
constexpr const char* TAG = "HLE";
} // namespace

void ModuleRegistry::register_module(const std::string& name, std::vector<std::string> exports,
                                     size_t implemented_count) {
    Module m;
    m.name = name;
    m.exports = std::move(exports);
    m.implemented_count = implemented_count;
    modules_[name] = std::move(m);
    ARMSX5_LOG_DEBUG(TAG, "registered module %s (%zu exports, %zu implemented)", name.c_str(),
                     m.exports.size(), implemented_count);
}

bool ModuleRegistry::has_module(const std::string& name) const { return modules_.count(name) != 0; }

const ModuleRegistry::Module* ModuleRegistry::module(const std::string& name) const {
    auto it = modules_.find(name);
    return it == modules_.end() ? nullptr : &it->second;
}

bool ModuleRegistry::record_unimplemented(const std::string& module_name,
                                          const std::string& export_name) {
    const std::string key = module_name + "::" + export_name;
    unimplemented_.push_back(key);
    ARMSX5_LOG_WARN(TAG, "unimplemented HLE call: %s (reported as error, not faked)",
                    key.c_str());
    return false;
}

} // namespace armsx5::system
