#pragma once
// RDNA2 (GFX10/10.3) shader-instruction decoder and opcode semantic table.
//
// Every field layout and opcode number in this module was verified against
// LLVM's AMDGPU MC encodings (llvm/test/MC/AMDGPU/*.s, hardware-validated
// byte sequences) and LLVM tablegen field definitions
// (VOPInstructions.td / SOPInstructions.td / EXPInstructions.td). The layout
// summary lives in docs/architecture/gpu.md.
//
// Scope: format classification, field extraction and a semantic table for
// the ALU/export subset. Memory families (DS/FLAT/MUBUF/MTBUF/MIMG/SMEM)
// are classified but not field-decoded yet; VOPC decodes opcode+src0 only.
#include "armsx5/core/result.h"

#include <expected>
#include <cstdint>
#include <span>

namespace armsx5::gpu::rdna2 {

enum class Format : uint8_t {
    Unknown = 0,
    SOP1,
    SOP2,
    SOPK,
    SOPC,
    SOPP,
    VOP1,
    VOP2,
    VOP3,  // e64 (8-byte) forms
    VOPC,
    EXP,
    // Classified only; field layout is pending verification.
    DS,
    FLAT,
    MUBUF,
    MTBUF,
    MIMG,
    SMEM,
};

// 32-bit instruction word count for a format (e64 families are 8 bytes).
uint32_t format_word_count(Format f);

// Semantic class of an opcode, used by the shader translator to route ops.
enum class OpClass : uint8_t {
    Other = 0,     // nop, flow control, barriers
    ScalarMove,    // s_mov*, s_movk*
    ScalarArith,   // SOP2 integer ALU (writes SCC on real hw)
    VectorMove,    // v_mov*
    VectorFloat,   // f32/f64 arithmetic
    VectorInt,     // integer arithmetic / bit logic
    ScalarBranch,  // s_branch, s_cbranch_*
    ProgramEnd,    // s_endpgm
    Export,        // exp
};

// One entry of the verified opcode table.
struct OpInfo {
    const char* name;       // canonical RDNA mnemonic, e.g. "V_ADD_F32"
    OpClass cls;
    uint8_t src_count;      // semantic source-operand count
    bool float_op;          // operates on float lanes
    bool translatable;      // SPIR-V emitter (v1) supports this opcode
};

// Looks up the verified semantic table. Returns nullptr for opcodes that are
// not in the curated subset (they still decode fine).
const OpInfo* lookup_op(Format format, uint32_t opcode);

// Decoded scalar-register operand encoding (the 8-bit literal fields shared
// by SOP and VOP scalar-mode sources). GFX10 table:
//   0-101   SGPR s0-s101
//   106/107 vcc lo/hi        124 m0      125 null      126/127 exec lo/hi
//   128-192 unsigned integer constants 0-64
//   193-208 negative integer constants -1..-16
//   240-247 float constants 0.5 -0.5 1.0 -1.0 2.0 -2.0 4.0 -4.0
//   255     inline literal (imm32 word follows the instruction)
struct Operand {
    enum class Kind : uint8_t {
        Null = 0,       // unused operand slot
        Sgpr,
        Vcc,
        M0,
        Exec,
        UintConst,      // index = constant value
        IntConst,       // index = negative constant magnitude (1..16)
        FloatConst,     // index into kFloatConsts
        Literal,        // imm32 follows (Instr::imm32)
        SgprVector,     // SGPR window s[n], s[n+1] (SOPK 64-bit dsts)
        Unknown,
    };
    Kind kind = Kind::Null;
    uint32_t index = 0;
    bool is_vector64 = false;  // b64 operand (SGPR pair)
};

const float* float_const_table();  // 8 entries, index 240..247
Operand decode_scalar_operand(uint32_t raw8);  // [7:0] encoding value

struct Instr {
    Format format = Format::Unknown;
    uint32_t opcode = 0;
    uint32_t dst = 0;         // sdst [22:16] (SOP*) or vdst [24:17]/[7:0] (VOP*)
    uint32_t src0 = 0;        // raw: VOP = bit8|VGPR-sel|[7:0]; SOP = [7:0]
    uint32_t src1 = 0;        // raw, same conventions (VOP3: [17:9])
    uint32_t src2 = 0;        // VOP3 only ([26:18])
    uint32_t imm16 = 0;       // SOPK/SOPP simm16
    uint32_t imm32 = 0;       // literal dword (VOP literal source / SOPK i32)
    bool two_word = false;    // instruction is 8 bytes
    bool literal_src = false; // some source encodes the 255 literal form
    bool src0_is_vgpr = false;
    bool src1_is_vgpr = false;
    bool src2_is_vgpr = false;
    bool clamp = false;       // VOP3 word0 [15]
    uint32_t omod = 0;        // VOP3 word1 [28:27]
    // EXP-specific:
    uint32_t en = 0;          // [3:0] output-enable mask
    uint32_t target = 0;      // [9:4]: 0-7 MRT0-7, 8 MRTZ, 9 NULL, 12 POS0,
                              // 20 PRIM, 32-63 PARAM0-31
    bool compr = false;       // [10]
    bool done = false;        // [11]
    bool vm = false;          // [12]
};

// Classifies and decodes an instruction. `words` must start at an
// instruction boundary; 8-byte families consume two words. Returns
// Unsupported for reserved encodings and InvalidArgument for a truncated
// instruction.
std::expected<Instr, Error> decode(std::span<const uint32_t> words);
std::expected<Instr, Error> decode(uint32_t word);  // 1-word convenience

const char* format_name(Format f);

// Family classification helper shared by the emitter.
bool format_is_8_byte(Format f);

} // namespace armsx5::gpu::rdna2
