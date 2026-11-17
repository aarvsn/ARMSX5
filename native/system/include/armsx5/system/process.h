#pragma once
// Process runtime: full pipeline for one guest program.
//   file -> ELF64 validation -> guest image mapping -> stack setup ->
//   CPU execution (interpreter, optional JIT acceleration) -> kernel syscalls
//
// This is the Phase-6 integration surface used by the Android frontend and
// the integration tests. Commercial SELF executables are NOT loadable yet;
// see docs/research/self-loading.md.
#include "armsx5/core/result.h"
#include "armsx5/loader/elf_loader.h"
#include "armsx5/system/kernel.h"

#include <expected>
#include <cstdint>
#include <string>

namespace armsx5::system {

struct RunResult {
    uint64_t exit_code = 0;
    bool exited = false;          // true when the guest called exit()
    Status status = Status::Ok;   // overall run status
    std::string detail;           // fault/unsupported message when applicable
    uint64_t fault_rip = 0;
    uint64_t steps = 0;
    uint64_t mapped_bytes = 0;
    loader::ExecutableMetadata metadata;
    std::string stdout_text;      // captured write(1/2) output
    KernelStats kernel_stats;
};

struct RunOptions {
    uint64_t max_steps = 100000000ull;
    uint64_t stack_size = 1u << 20; // 1 MiB guest stack
    bool use_jit = false;           // JIT acceleration (aarch64 hosts only)
};

class ProcessRuntime {
public:
    explicit ProcessRuntime(const RunOptions& options = {});

    // Runs an in-memory executable image.
    std::expected<RunResult, Error> run(const uint8_t* data, size_t size);

    // Convenience overload: loads a file from the host filesystem.
    std::expected<RunResult, Error> run_file(const std::string& path);

    CaptureSink& sink() { return sink_; }

private:
    RunOptions options_;
    memory::GuestMemory mem_;
    CaptureSink sink_;
    Kernel kernel_;
};

} // namespace armsx5::system
