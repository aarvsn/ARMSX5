#pragma once
// ARM64 JIT for the ARMSX5 CPU engine.
//
// v1 policy (docs/architecture/cpu.md):
//  - Integer ALU/MOV/IMM fast paths are emitted as native AArch64 code.
//  - Memory access, flags, and control flow go through host helper calls:
//    correctness before performance (Phase 3 acceptance criteria).
//  - The interpreter remains the correctness reference. Blocks that contain
//    anything the JIT cannot translate simply end before it, and execution
//    falls back to the interpreter — unsupported instructions are never
//    translated incorrectly or silently mis-executed.
//  - Generated code executes only on aarch64 hosts. On x86-64 development
//    hosts the JIT still performs real translation (tested for encoding and
//    cache behavior) but does not execute guest code.
#include "armsx5/core/result.h"
#include "armsx5/cpu/cpu_state.h"
#include "armsx5/cpu/interpreter.h"
#include "armsx5/cpu/ir.h"
#include "armsx5/platform/exec_arena.h"

#include <expected>
#include <cstdint>
#include <unordered_map>

namespace armsx5::cpu {

class Jit {
public:
    struct Stats {
        uint64_t blocks_cached = 0;
        uint64_t translations = 0;
        uint64_t invalidations = 0;
        uint64_t helper_exits = 0;
    };

    struct Block {
        uint64_t guest_start = 0;
        uint64_t guest_end = 0;
        size_t code_index = 0; // offset into the exec arena (in words)
        size_t code_words = 0;
    };

    explicit Jit(memory::GuestMemory& mem, ISyscallHandler* syscall_handler = nullptr);

    std::expected<void, Error> init();

    // Returns the translated block starting at (or cached at) `rip`. The page
    // must be marked executable in the guest memory manager.
    std::expected<const Block*, Error> get_block(uint64_t rip);

    // Executes one block on aarch64 hosts. On other hosts returns
    // Status::Unsupported so callers fall back to the interpreter.
    std::expected<uint64_t, Error> execute_block(const Block& block, CpuState& state);

    void invalidate_all();
    void invalidate_range(uint64_t base, size_t size);

    const Stats& stats() const { return stats_; }
    static bool can_execute_natively();
    static const char* result_kind_name(uint8_t kind);
    // True when the last executed block ended because the guest called exit().
    bool consume_stop() {
        const bool s = last_run_stopped_;
        last_run_stopped_ = false;
        return s;
    }

private:
    static constexpr size_t kMaxBlockInstrs = 64;
    static constexpr size_t kMaxBlockBytes = 4096;

    memory::GuestMemory& mem_;
    ISyscallHandler* syscall_handler_;
    platform::ExecArena arena_;
    std::unordered_map<uint64_t, Block> cache_;
    Stats stats_;
    bool last_run_stopped_ = false;

    std::expected<ir::Block, Error> build_ir(uint64_t rip);
    std::expected<size_t, Error> translate(const ir::Block& ir_block);
};

} // namespace armsx5::cpu
