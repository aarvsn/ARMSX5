#pragma once
// x86-64 (long mode) instruction decoder for the ARMSX5 CPU engine.
//
// Implemented subset: common integer instructions (ALU block, MOV, LEA,
// stack ops, control flow, SETcc/CMOVcc, MOVZX/MOVSX/MOVSD, shifts,
// MUL/IMUL/DIV/IDIV, NEG/NOT/INC/DEC, XCHG, SYSCALL, NOPs, CDQ/CDQE/CQO)
// plus an SSE subset (scalar/packed float moves and arithmetic, comparisons,
// conversions, COMIS*/UCOMIS*, packed integer add/sub/mul, PSHUFD) and an
// AVX subset (VEX.128/VEX.256 mirrors of the same operations, with the
// three-operand non-destructive AVX semantics). String ops, bit tests and
// privileged instructions decode as Unsupported so the runtime reports
// them instead of mis-executing.
#include "armsx5/core/result.h"

#include <expected>
#include <cstddef>
#include <cstdint>

namespace armsx5::cpu::x86 {

enum class OperandKind : uint8_t { None = 0, Reg = 1, Mem = 2, Imm = 3 };

struct MemOperand {
    uint8_t base = 0xFF;  // reg index, 0xFF = absent
    uint8_t index = 0xFF; // reg index, 0xFF = absent
    uint8_t scale = 1;    // 1,2,4,8
    int64_t disp = 0;     // sign-extended displacement
    bool rip_relative = false;
    bool valid = false;
};

struct Operand {
    OperandKind kind = OperandKind::None;
    uint8_t reg = 0;      // resolved reg index (REX-applied); xmm index for SSE forms
    bool high_byte = false;
    MemOperand mem{};
    int64_t imm = 0;      // sign-extended immediate
    uint8_t size = 0;     // operand size in bytes (16 = xmm)
};

enum class InstrClass : uint8_t {
    Invalid = 0,
    Alu,        // dst,src forms + dst,imm forms (alu_op selects ADD..CMP)
    Mov,
    MovImm,
    Lea,
    Push,
    Pop,
    Call,       // call r/m64
    CallRel,
    Ret,
    Jcc,
    JmpRel,
    JmpReg,
    Setcc,
    Cmovcc,
    Test,
    Shift,      // shift_op selects ROL..SAR
    Mul,        // one-operand MUL
    IMul,       // 1/2/3-operand IMUL
    Div,
    Neg,
    Not,
    IncDec,
    Movzx,
    Movsx,
    Movsxd,
    Xchg,
    SignExtend, // CDQE / CDQ / CQO
    Syscall,
    Nop,
    SseMov,     // MOVSS/MOVSD/MOVUPS/MOVAPS/MOVDQA/MOVDQU/MOVD/MOVQ
    SseArith,   // ADD/SUB/MUL/DIV/SQRT/AND/OR/XOR/ANDN (sse_form + sse_op)
    SseCmp,     // CMPSS/CMPPS/CMPSD/CMPPD (sse_pred)
    SseComis,   // COMISS/UCOMISS/COMISD/UCOMISD (flag semantics)
    SseCvt,     // CVTSI2SS/CVTTSS2SI/CVTSS2SI/CVTDQ2PS/CVTPS2DQ/... (sse_op)
    SseInt,     // PADD*/PSUB*/PMULLW/PMULUDQ (sse_op)
    SseShuffle, // PSHUFD (imm3)
};

enum class AluOp : uint8_t { Add = 0, Or = 1, Adc = 2, Sbb = 3, And = 4, Sub = 5, Xor = 6, Cmp = 7 };
enum class ShiftOp : uint8_t { Rol = 0, Ror = 1, Rcl = 2, Rcr = 3, Shl = 4, Shr = 5, Sal = 6, Sar = 7 };

// SSE operand-size/form selector.
enum class SseForm : uint8_t {
    SS = 0,   // scalar f32 (F3 prefix)
    PS = 1,   // packed 4x f32 (no prefix)
    SD = 2,   // scalar f64 (F2 prefix)
    PD = 3,   // packed 2x f64 (66 prefix)
    DQA = 4,  // aligned 128-bit int (66)
    DQU = 5,  // unaligned 128-bit int (F3)
    D = 6,    // 32-bit GPR/xmm bridge (66 MOVD)
    Q = 7,    // 64-bit GPR/xmm bridge (66 REX.W MOVD; also 66 0F D6 stores)
    QF3 = 8,  // F3 0F 7E MOVQ xmm, xmm/m64 (zero-extends into xmm dst)
    QD6 = 9,  // 66 0F D6 MOVQ xmm/m64, xmm (store low 64; upper preserved)
    AP = 10,  // MOVAPS/MOVAPD: full 128-bit copy, 16-byte aligned
};

// SSE arithmetic/op selectors (interpreter dispatch).
enum class SseOp : uint8_t {
    Add = 0, Sub = 1, Mul = 2, Div = 3, Sqrt = 4,
    And = 5, Or = 6, Xor = 7, Andn = 8,
    // conversions
    IntToF32 = 9,  // CVTSI2SS
    IntToF64 = 10, // CVTSI2SD
    TruncF32ToI = 11, // CVTTSS2SI
    TruncF64ToI = 12, // CVTTSD2SI
    RoundF32ToI = 13, // CVTSS2SI
    RoundF64ToI = 14, // CVTSD2SI
    I32ToF32x4 = 15,  // CVTDQ2PS
    F32ToI32x4 = 16,  // CVTPS2DQ
    TruncF32ToI32x4 = 17, // CVTTPS2DQ
    // packed integer
    PaddB = 18, PaddW = 19, PaddD = 20, PaddQ = 21,
    PsubB = 22, PsubW = 23, PsubD = 24, PsubQ = 25,
    Pmullw = 26, Pmuludq = 27,
};

struct Decoded {
    InstrClass cls = InstrClass::Invalid;
    uint8_t opsize = 4;   // 1/2/4/8 bytes (16 = full xmm form, 32 = ymm form)
    Operand dst{};
    Operand src{};
    AluOp alu_op = AluOp::Add;
    ShiftOp shift_op = ShiftOp::Shl;
    SseForm sse_form = SseForm::PS;
    SseOp sse_op = SseOp::Add;
    uint8_t sse_pred = 0; // CMPSS/CMPPS predicate 0..7
    uint8_t cond = 0;     // 0..15 x86 condition code
    uint8_t imul_form = 0; // IMUL: 0=one-operand, 1=two-operand, 2=three-operand
    int64_t rel = 0;      // branch/call relative displacement
    int64_t imm3 = 0;     // IMUL three-operand immediate / PSHUFD selector
    uint8_t length = 0;   // total encoded length

    // AVX (VEX) fields. Valid only when vex == true.
    bool vex = false;     // instruction is VEX-encoded (0xC4/0xC5 prefix)
    bool vex_l = false;   // VEX.L: 256-bit (YMM) vector length
    uint8_t vvvv = 0;     // decoded VEX.vvvv register index 0..15 (already
                          // complement-decoded); 15 can never occur because
                          // 1111b decodes to 0, so "v unused" ops simply
                          // ignore this field (lenient, unlike hardware #UD)
};

// Decodes one instruction. On success out.length is set. Unsupported
// instructions return Status::Unsupported (never silently mis-executed).
std::expected<Decoded, Error> decode(const uint8_t* code, size_t max_len);

const char* class_name(InstrClass cls);
const char* alu_name(AluOp op);
const char* cond_name(uint8_t cond);
const char* sse_pred_name(uint8_t pred);

} // namespace armsx5::cpu::x86
