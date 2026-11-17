#pragma once
// ARMSX5 x86-64 interpreter — the correctness reference execution engine.
//
// The interpreter decodes and executes one guest instruction at a time
// against the guest memory manager. Every memory access is bounds- and
// permission-checked; unsupported instructions stop execution with
// Status::Unsupported instead of mis-executing.
//
// The ARM64 JIT (cpu/jit) must match this engine's architectural results.
// See docs/development/testing.md for the differential-testing policy.
#include "armsx5/core/result.h"
#include "armsx5/cpu/cpu_state.h"
#include "armsx5/cpu/x86/decoder.h"
#include "armsx5/memory/guest_memory.h"

#include <expected>
#include <cstdint>
#include <string>

namespace armsx5::cpu {

// Implemented by the system kernel to service the SYSCALL instruction.
class ISyscallHandler {
public:
    virtual ~ISyscallHandler() = default;
    // Reads the syscall number from state (RAX) and arguments from
    // RDI/RSI/RDX/R10/R8/R9 per the x86-64 SysV calling convention. Writes
    // the result into RAX (and CF per FreeBSD semantics). Set `stop` to end
    // the run loop (e.g. guest exit()).
    virtual Status on_syscall(CpuState& state, bool& stop) = 0;
};

struct RunStats {
    enum class Stop { Exited, MaxSteps, Fault, Unsupported } stop = Stop::MaxSteps;
    uint64_t steps = 0;
    uint64_t exit_code = 0;
    Status status = Status::Ok;
    uint64_t fault_rip = 0;
    std::string message;
};

class Interpreter {
public:
    explicit Interpreter(memory::GuestMemory& mem);

    CpuState& state() { return state_; }
    const CpuState& state() const { return state_; }
    void set_syscall_handler(ISyscallHandler* handler) { syscall_ = handler; }
    // True after a syscall handler requested termination (guest exit()).
    bool stop_requested() const { return run_stop_; }

    // Executes a single instruction at the current RIP.
    std::expected<void, Error> step();

    // Runs until exit, fault, unsupported instruction, or step budget.
    RunStats run(uint64_t max_steps);

    static const char* stop_name(RunStats::Stop stop);

private:
    memory::GuestMemory& mem_;
    CpuState state_;
    ISyscallHandler* syscall_ = nullptr;
    bool run_stop_ = false; // set by the syscall handler (guest exit)

    // ---- helpers (defined in interpreter.cpp) ----
    uint64_t compute_addr(const x86::MemOperand& m, uint8_t instr_length) const;
    std::expected<uint64_t, Error> read_operand(const x86::Operand& op,
                                                const x86::Decoded& d) const;
    std::expected<void, Error> write_operand(const x86::Operand& op, uint64_t value,
                                             const x86::Decoded& d);
    std::expected<uint64_t, Error> read_mem_sized(uint64_t addr, uint8_t size) const;
    std::expected<void, Error> write_mem_sized(uint64_t addr, uint64_t value, uint8_t size);
    std::expected<XmmValue, Error> read_mem128(uint64_t addr) const;
    std::expected<void, Error> write_mem128(uint64_t addr, const XmmValue& v);
    // AVX vector state access. read_vec(reg, false) returns the XMM
    // register; read_vec(reg, true) the full YMM register. write_vec with
    // wide=false applies the VEX.128 destination rule: the upper 128 bits
    // of the YMM register are zeroed (verified against hardware; this holds
    // for every instruction implemented here, including VMOVSS/VMOVSD).
    Vec256 read_vec(uint8_t reg, bool wide) const;
    void write_vec(uint8_t reg, bool wide, const Vec256& v);
    std::expected<Vec256, Error> read_mem256(uint64_t addr) const;
    std::expected<void, Error> write_mem256(uint64_t addr, const Vec256& v);
    std::expected<void, Error> push(uint64_t value, uint8_t size);
    std::expected<uint64_t, Error> pop(uint8_t size);
    std::expected<void, Error> execute(x86::Decoded& d);
    // SSE sub-executors (SSE execution lives in interpreter_sse.cpp).
    std::expected<XmmValue, Error> sse_load_source(const x86::Decoded& d);
    std::expected<void, Error> execute_sse_mov(x86::Decoded& d);
    std::expected<void, Error> execute_sse_arith(x86::Decoded& d);
    std::expected<void, Error> execute_sse_cmp(x86::Decoded& d);
    std::expected<void, Error> execute_sse_comis(x86::Decoded& d);
    std::expected<void, Error> execute_sse_cvt(x86::Decoded& d);
    std::expected<void, Error> execute_sse_int(x86::Decoded& d);
    std::expected<void, Error> execute_sse_shuffle(x86::Decoded& d);
    // AVX sub-executors (AVX execution lives in interpreter_avx.cpp). The
    // three-operand layout is dst = reg field, src1 = vvvv (d.vvvv), src2 =
    // rm; semantics verified against real AVX hardware (see
    // docs/architecture/cpu.md for the rules this implements).
    std::expected<Vec256, Error> avx_load_source(const x86::Decoded& d);
    std::expected<void, Error> execute_avx_mov(x86::Decoded& d);
    std::expected<void, Error> execute_avx_arith(x86::Decoded& d);
    std::expected<void, Error> execute_avx_cmp(x86::Decoded& d);
    std::expected<void, Error> execute_avx_comis(x86::Decoded& d);
    std::expected<void, Error> execute_avx_cvt(x86::Decoded& d);
    std::expected<void, Error> execute_avx_int(x86::Decoded& d);
    std::expected<void, Error> execute_avx_shuffle(x86::Decoded& d);
};

} // namespace armsx5::cpu
