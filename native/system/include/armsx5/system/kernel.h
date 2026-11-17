#pragma once
// PS5 kernel emulation framework.
//
// v1 scope: syscall dispatch over the documented FreeBSD/amd64 baseline
// (exit, read, write, open, close, mmap, munmap, mprotect, sched_yield),
// with every unsupported syscall returning a documented error (FreeBSD
// ENOSYS + CF=1) and being counted for diagnostics. Guest threading
// (thr_new) is registered but not scheduled yet — see docs/architecture/
// system.md for the blocking analysis.
#include "armsx5/core/result.h"
#include "armsx5/cpu/cpu_state.h"
#include "armsx5/cpu/interpreter.h"
#include "armsx5/memory/guest_memory.h"

#include <cstdint>
#include <string>

namespace armsx5::system {

// Where guest write(1/2, ...) output goes.
class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual void write(const uint8_t* data, size_t len) = 0;
};

class StderrSink : public OutputSink {
public:
    void write(const uint8_t* data, size_t len) override;
};

class CaptureSink : public OutputSink {
public:
    void write(const uint8_t* data, size_t len) override;
    const std::string& captured() const { return captured_; }
    void clear() { captured_.clear(); }

private:
    std::string captured_;
};

struct KernelStats {
    uint64_t write_calls = 0;
    uint64_t mmap_calls = 0;
    uint64_t munmap_calls = 0;
    uint64_t mprotect_calls = 0;
    uint64_t yield_calls = 0;
    uint64_t unsupported_calls = 0;
    uint64_t failed_calls = 0;
};

class Kernel : public cpu::ISyscallHandler {
public:
    explicit Kernel(memory::GuestMemory& mem);

    void set_output_sink(OutputSink* sink) { sink_ = sink; }
    void set_last_exit_code(uint64_t code) { last_exit_code_ = code; }
    uint64_t last_exit_code() const { return last_exit_code_; }
    const KernelStats& stats() const { return stats_; }

    // ISyscallHandler: reads RAX=nr, RDI/RSI/RDX/R10/R8/R9 args. Results in
    // RAX; FreeBSD error convention sets the carry flag (CF) with errno.
    Status on_syscall(cpu::CpuState& state, bool& stop) override;

private:
    static OutputSink& default_sink();

    memory::GuestMemory& mem_;
    OutputSink* sink_;
    KernelStats stats_;
    uint64_t last_exit_code_ = 0;
};

} // namespace armsx5::system
