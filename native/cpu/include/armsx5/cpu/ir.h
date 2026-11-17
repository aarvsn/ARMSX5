#pragma once
// Minimal linear IR used by the ARM64 JIT.
//
// v1 scope: the JIT fast-paths integer ALU/MOV/IMM and delegates memory
// access, flags and control flow to host helpers for correctness. Blocks
// end at the first unsupported instruction (the interpreter handles those).
#include <cstdint>
#include <vector>

namespace armsx5::cpu::ir {

enum class Op : uint8_t {
    Imm,      // dst = imm
    Mov,      // dst = a
    Bin,      // dst = a <bin> b        (bin: Add/Sub/And/Or/Xor)
    Shift,    // dst = a <shift> imm    (shift: Shl/Shr/Sar, imm pre-masked)
    Lea,      // dst = a + imm          (simple base+disp only in v1)
    LoadMem,  // dst = [a + disp]       (helper)
    StoreMem, // [a + disp] = b         (helper)
    CmpImm,   // flags = a - imm        (helper)
    CmpReg,   // flags = a - b          (helper)
    Jcc,      // terminator
    Jmp,      // terminator
    Ret,      // terminator
    CallRel,  // terminator
    Syscall,  // terminator
    Nop,
};

enum class BinOp : uint8_t { Add = 0, Sub = 1, And = 2, Or = 3, Xor = 4 };
enum class ShiftKind : uint8_t { Shl = 0, Shr = 1, Sar = 2 };

struct Instr {
    Op op;
    uint8_t dst = 0xFF; // guest GPR index
    uint8_t a = 0xFF;
    uint8_t b = 0xFF;
    uint8_t size = 8;
    int64_t imm = 0;
    int64_t disp = 0;
    uint64_t next_rip = 0; // rip following the guest instruction (helper arg)
    BinOp bin = BinOp::Add;
    ShiftKind shift = ShiftKind::Shl;
    uint8_t cond = 0;
    int64_t rel = 0;
    uint8_t guest_len = 0; // encoded x86 length of the source instruction
};

struct Block {
    uint64_t start_rip = 0;
    uint64_t end_rip = 0; // rip after last instruction
    std::vector<Instr> instrs;
};

} // namespace armsx5::cpu::ir
