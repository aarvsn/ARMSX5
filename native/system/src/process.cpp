#include "armsx5/system/process.h"

#include "armsx5/core/log.h"
#include "armsx5/cpu/cpu_state.h"
#include "armsx5/cpu/interpreter.h"
#include "armsx5/cpu/jit.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace armsx5::system {
namespace {
constexpr const char* TAG = "Process";

constexpr uint64_t kGuestAddressSpace = 0x100000000ull; // 4 GiB

// Reads a whole host file into memory.
std::expected<std::vector<uint8_t>, Error> read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::unexpected(Error{Status::IoError, "cannot open " + path});
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > (64 << 20)) { // 64 MiB sanity cap
        fclose(f);
        return std::unexpected(Error{Status::IoError, "invalid file size for " + path});
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (size > 0 && fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        fclose(f);
        return std::unexpected(Error{Status::IoError, "short read on " + path});
    }
    fclose(f);
    return buf;
}
} // namespace

ProcessRuntime::ProcessRuntime(const RunOptions& options)
    : options_(options), mem_(kGuestAddressSpace), kernel_(mem_) {
    kernel_.set_output_sink(&sink_);
}

std::expected<RunResult, Error> ProcessRuntime::run_file(const std::string& path) {
    auto data = read_file(path);
    if (!data) return std::unexpected(data.error());
    return run(data->data(), data->size());
}

std::expected<RunResult, Error> ProcessRuntime::run(const uint8_t* data, size_t size) {
    RunResult result;

    // 1. Validate + inspect.
    auto meta = loader::ElfLoader::inspect(data, size);
    if (!meta) return std::unexpected(meta.error());
    result.metadata = *meta;

    // 2. Map the image.
    auto img = loader::ElfLoader::load(mem_, data, size);
    if (!img) return std::unexpected(img.error());

    // 3. Allocate a guest stack and prime the initial machine state.
    auto stack = mem_.allocate(static_cast<size_t>(options_.stack_size), 16,
                               memory::Prot::R | memory::Prot::W);
    if (!stack) return std::unexpected(stack.error());
    const uint64_t stack_top = (*stack + options_.stack_size) & ~0xFULL;

    cpu::CpuState state;
    state.rip = img->entry;
    state.gpr[cpu::reg::SP] = stack_top;
    // Minimal System-V initial stack: argc=0, argv=NULL, envp=NULL,
    // followed by a null return address.
    const uint64_t zero = 0;
    for (int i = 0; i < 4; ++i) {
        state.gpr[cpu::reg::SP] -= 8;
        auto w = mem_.write(state.gpr[cpu::reg::SP], &zero, 8);
        if (!w) return std::unexpected(w.error());
    }

    // 4. Execute.
    cpu::Interpreter interp(mem_);
    interp.set_syscall_handler(&kernel_);
    interp.state() = state;

    if (options_.use_jit && cpu::Jit::can_execute_natively()) {
        cpu::Jit jit(mem_, &kernel_);
        auto jinit = jit.init();
        if (!jinit) return std::unexpected(jinit.error());
        ARMSX5_LOG_INFO(TAG, "JIT acceleration enabled (aarch64)");
        uint64_t executed = 0;
        bool stop = false;
        while (!stop && executed < options_.max_steps) {
            auto block = jit.get_block(interp.state().rip);
            if (!block) {
                // Fall back to the interpreter for this instruction.
                auto r = interp.step();
                ++executed;
                if (!r) break;
                if (interp.stop_requested()) stop = true;
                continue;
            }
            auto r = jit.execute_block(**block, interp.state());
            ++executed;
            if (!r) break;
            if (jit.consume_stop()) stop = true;
        }
        result.steps = executed;
        result.exited = stop;
        result.exit_code = kernel_.last_exit_code();
    } else {
        auto stats = interp.run(options_.max_steps);
        result.steps = stats.steps;
        result.exited = stats.stop == cpu::RunStats::Stop::Exited;
        result.exit_code = stats.exit_code;
        result.fault_rip = stats.fault_rip;
        if (stats.stop == cpu::RunStats::Stop::Fault ||
            stats.stop == cpu::RunStats::Stop::Unsupported) {
            result.status = stats.status;
            result.detail = std::string(cpu::Interpreter::stop_name(stats.stop)) + ": " +
                            stats.message + " @ rip=0x" +
                            std::to_string(stats.fault_rip);
        }
    }

    result.stdout_text = sink_.captured();
    result.kernel_stats = kernel_.stats();
    result.mapped_bytes = mem_.mapped_bytes();
    ARMSX5_LOG_INFO(TAG, "run finished: exited=%d exit_code=%llu steps=%llu mapped=%lluKB",
                    result.exited ? 1 : 0, static_cast<unsigned long long>(result.exit_code),
                    static_cast<unsigned long long>(result.steps),
                    static_cast<unsigned long long>(result.mapped_bytes / 1024));
    return result;
}

} // namespace armsx5::system
