#pragma once
// HLE module registration framework.
//
// PS5 system libraries (libSce*.sprx) are served through HLE: modules are
// registered with their export names, and calls to exports that have no
// implementation yet are recorded as "unimplemented traps" with full
// diagnostics instead of silently succeeding. No fabricated return values.
#include <map>
#include <string>
#include <vector>

namespace armsx5::system {

class ModuleRegistry {
public:
    struct Module {
        std::string name;
        std::vector<std::string> exports;
        size_t implemented_count = 0; // exports backed by real HLE code
    };

    // Registers a module; re-registration with the same name replaces it.
    void register_module(const std::string& name, std::vector<std::string> exports,
                         size_t implemented_count = 0);

    bool has_module(const std::string& name) const;
    const Module* module(const std::string& name) const;
    size_t module_count() const { return modules_.size(); }

    // Records an export call that has no implementation. Returns false so
    // callers (future guest-call dispatch) surface a documented error.
    bool record_unimplemented(const std::string& module, const std::string& export_name);
    const std::vector<std::string>& unimplemented_calls() const { return unimplemented_; }

private:
    std::map<std::string, Module> modules_;
    std::vector<std::string> unimplemented_;
};

} // namespace armsx5::system
