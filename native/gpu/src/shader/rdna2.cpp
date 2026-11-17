#include "armsx5/gpu/shader/rdna2.h"

#include <array>
#include <unordered_map>

namespace armsx5::gpu::rdna2 {
namespace {

// ---------------------------------------------------------------------------
// Family detection (GFX10/10.3). Verified against LLVM MC encodings:
//
//  bit31 == 0:
//    op6 = [30:25]: 0-61 VOP2, 62 VOPC, 63 VOP1
//  bit31 == 1, top6 = [31:26]:
//    53 VOP3 (8 bytes)     54 DS       55 FLAT    56 MUBUF
//    58 MTBUF              60 MIMG     61 SMEM    62 EXP (8 bytes)
//    top4 = 0b1011 (44-47): v = [28:23]
//      61 SOP1   62 SOPC   63 SOPP   else (<32) SOPK
//    top6 in 32-43: SOP2 (op = [29:23])
// ---------------------------------------------------------------------------
struct Family {
    Format format;
    bool ok;
};

constexpr uint32_t kBit31 = 0x80000000u;

Family classify_word0(uint32_t w) {
    if ((w & kBit31) == 0) {
        const uint32_t op6 = (w >> 25) & 0x3F;
        if (op6 == 63) return {Format::VOP1, true};
        if (op6 == 62) return {Format::VOPC, true};
        return {Format::VOP2, true};
    }
    const uint32_t top6 = (w >> 26) & 0x3F;
    switch (top6) {
    case 53: return {Format::VOP3, true};
    case 54: return {Format::DS, true};
    case 55: return {Format::FLAT, true};
    case 56: return {Format::MUBUF, true};
    case 58: return {Format::MTBUF, true};
    case 60: return {Format::MIMG, true};
    case 61: return {Format::SMEM, true};
    case 62: return {Format::EXP, true};
    default: break;
    }
    const uint32_t top4 = (w >> 28) & 0xF;
    if (top4 == 0xB) {
        // v = bits[28:23]: SOP1/SOPC/SOPP occupy 61/62/63; SOPK words are
        // 0b1_00000..0b1_11100 (bit28 fixed 1, op = v - 32 in 0..28).
        const uint32_t v = (w >> 23) & 0x3F;
        if (v == 61) return {Format::SOP1, true};
        if (v == 62) return {Format::SOPC, true};
        if (v == 63) return {Format::SOPP, true};
        if (v >= 32 && v < 61) return {Format::SOPK, true};
        return {Format::Unknown, false};
    }
    if (top6 >= 32 && top6 <= 43) return {Format::SOP2, true};
    return {Format::Unknown, false};
}

// ---------------------------------------------------------------------------
// Verified opcode tables (GFX10/10.3). Sources:
//  - VOP2: llvm VOP2Instructions.td GFX10 block (bits<6> op), cross-checked
//    against gfx10_asm_vop2.s literal encodings.
//  - VOP1: llvm VOP1Instructions.td (bits<8> op, prefix [31:25] = 0x3F).
//  - VOP3: llvm VOP3Instructions.td (bits<10> op at [25:16]). VOP2-class ops
//    appear at 0x100 + VOP2 opcode (VOP3e_gfx10<{0,1,0,0,op{5-0}}>).
//  - SOP1/SOP2/SOPK/SOPP: llvm SOPInstructions.td GFX10 blocks.
// ---------------------------------------------------------------------------
const std::unordered_map<uint32_t, OpInfo>& sop1_table() {
    static const std::unordered_map<uint32_t, OpInfo> table = {
        {0x03, {"S_MOV_B32", OpClass::ScalarMove, 1, false, true}},
        {0x04, {"S_MOV_B64", OpClass::ScalarMove, 1, false, false}},
        {0x05, {"S_CMOV_B32", OpClass::ScalarArith, 1, false, false}},
        {0x06, {"S_CMOV_B64", OpClass::ScalarArith, 1, false, false}},
        {0x07, {"S_NOT_B32", OpClass::ScalarArith, 1, false, true}},
        {0x1F, {"S_GETPC_B64", OpClass::Other, 0, false, false}},
        {0x20, {"S_SETPC_B64", OpClass::Other, 1, false, false}},
        {0x21, {"S_SWAPPC_B64", OpClass::Other, 1, false, false}},
    };
    return table;
}

const std::unordered_map<uint32_t, OpInfo>& sop2_table() {
    static const std::unordered_map<uint32_t, OpInfo> table = {
        {0x00, {"S_ADD_U32", OpClass::ScalarArith, 2, false, true}},
        {0x01, {"S_SUB_U32", OpClass::ScalarArith, 2, false, true}},
        {0x02, {"S_ADD_I32", OpClass::ScalarArith, 2, false, true}},
        {0x03, {"S_SUB_I32", OpClass::ScalarArith, 2, false, true}},
        {0x04, {"S_ADDC_U32", OpClass::ScalarArith, 2, false, false}},
        {0x05, {"S_SUBB_U32", OpClass::ScalarArith, 2, false, false}},
        {0x06, {"S_MIN_I32", OpClass::ScalarArith, 2, false, true}},
        {0x07, {"S_MIN_U32", OpClass::ScalarArith, 2, false, true}},
        {0x08, {"S_MAX_I32", OpClass::ScalarArith, 2, false, true}},
        {0x09, {"S_MAX_U32", OpClass::ScalarArith, 2, false, true}},
        {0x0A, {"S_CSELECT_B32", OpClass::ScalarArith, 2, false, false}},
        {0x0E, {"S_AND_B32", OpClass::ScalarArith, 2, false, true}},
        {0x10, {"S_OR_B32", OpClass::ScalarArith, 2, false, true}},
        {0x12, {"S_XOR_B32", OpClass::ScalarArith, 2, false, true}},
        {0x1E, {"S_LSHL_B32", OpClass::ScalarArith, 2, false, true}},
        {0x20, {"S_LSHR_B32", OpClass::ScalarArith, 2, false, true}},
        {0x22, {"S_ASHR_I32", OpClass::ScalarArith, 2, false, true}},
        {0x26, {"S_MUL_I32", OpClass::ScalarArith, 2, false, true}},
    };
    return table;
}

const std::unordered_map<uint32_t, OpInfo>& sopk_table() {
    static const std::unordered_map<uint32_t, OpInfo> table = {
        {0x00, {"S_MOVK_I32", OpClass::ScalarMove, 1, false, true}},
        {0x0F, {"S_ADDK_I32", OpClass::ScalarArith, 1, false, false}},
        {0x16, {"S_CALL_B64", OpClass::Other, 1, false, false}},
    };
    return table;
}

const std::unordered_map<uint32_t, OpInfo>& sopp_table() {
    static const std::unordered_map<uint32_t, OpInfo> table = {
        {0x00, {"S_NOP", OpClass::Other, 0, false, true}},
        {0x01, {"S_ENDPGM", OpClass::ProgramEnd, 0, false, true}},
        {0x02, {"S_BRANCH", OpClass::ScalarBranch, 1, false, false}},
        {0x04, {"S_CBRANCH_SCC0", OpClass::ScalarBranch, 1, false, false}},
        {0x05, {"S_CBRANCH_SCC1", OpClass::ScalarBranch, 1, false, false}},
        {0x08, {"S_CBRANCH_EXECZ", OpClass::ScalarBranch, 1, false, false}},
        {0x09, {"S_CBRANCH_EXECNZ", OpClass::ScalarBranch, 1, false, false}},
    };
    return table;
}

const std::unordered_map<uint32_t, OpInfo>& vop1_table() {
    static const std::unordered_map<uint32_t, OpInfo> table = {
        {0x00, {"V_NOP", OpClass::Other, 0, false, true}},
        {0x01, {"V_MOV_B32", OpClass::VectorMove, 1, false, true}},
        {0x02, {"V_READFIRSTLANE_B32", OpClass::VectorMove, 1, false, false}},
        {0x05, {"V_CVT_F32_I32", OpClass::VectorFloat, 1, true, true}},
        {0x06, {"V_CVT_F32_U32", OpClass::VectorFloat, 1, true, true}},
        {0x07, {"V_CVT_U32_F32", OpClass::VectorInt, 1, false, true}},
        {0x08, {"V_CVT_I32_F32", OpClass::VectorInt, 1, false, true}},
        {0x21, {"V_TRUNC_F32", OpClass::VectorFloat, 1, true, true}},
        {0x22, {"V_CEIL_F32", OpClass::VectorFloat, 1, true, true}},
        {0x23, {"V_RNDNE_F32", OpClass::VectorFloat, 1, true, true}},
        {0x24, {"V_FLOOR_F32", OpClass::VectorFloat, 1, true, true}},
        {0x2A, {"V_RCP_F32", OpClass::VectorFloat, 1, true, true}},
        {0x33, {"V_SQRT_F32", OpClass::VectorFloat, 1, true, true}},
        {0x37, {"V_NOT_B32", OpClass::VectorInt, 1, false, true}},
        {0x3A, {"V_FFBL_B32", OpClass::VectorInt, 1, false, false}},
    };
    return table;
}

const std::unordered_map<uint32_t, OpInfo>& vop2_table() {
    static const std::unordered_map<uint32_t, OpInfo> table = {
        {0x01, {"V_CNDMASK_B32", OpClass::VectorInt, 2, false, false}},
        {0x03, {"V_ADD_F32", OpClass::VectorFloat, 2, true, true}},
        {0x04, {"V_SUB_F32", OpClass::VectorFloat, 2, true, true}},
        {0x05, {"V_SUBREV_F32", OpClass::VectorFloat, 2, true, true}},
        {0x06, {"V_MAC_LEGACY_F32", OpClass::VectorFloat, 3, true, false}},
        {0x07, {"V_MUL_LEGACY_F32", OpClass::VectorFloat, 2, true, true}},
        {0x08, {"V_MUL_F32", OpClass::VectorFloat, 2, true, true}},
        {0x0B, {"V_MUL_U32_U24", OpClass::VectorInt, 2, false, false}},
        {0x0F, {"V_MIN_F32", OpClass::VectorFloat, 2, true, true}},
        {0x10, {"V_MAX_F32", OpClass::VectorFloat, 2, true, true}},
        {0x11, {"V_MIN_I32", OpClass::VectorInt, 2, false, true}},
        {0x12, {"V_MAX_I32", OpClass::VectorInt, 2, false, true}},
        {0x13, {"V_MIN_U32", OpClass::VectorInt, 2, false, true}},
        {0x14, {"V_MAX_U32", OpClass::VectorInt, 2, false, true}},
        {0x16, {"V_LSHRREV_B32", OpClass::VectorInt, 2, false, true}},
        {0x18, {"V_ASHRREV_I32", OpClass::VectorInt, 2, false, true}},
        {0x1A, {"V_LSHLREV_B32", OpClass::VectorInt, 2, false, true}},
        {0x1B, {"V_AND_B32", OpClass::VectorInt, 2, false, true}},
        {0x1C, {"V_OR_B32", OpClass::VectorInt, 2, false, true}},
        {0x1D, {"V_XOR_B32", OpClass::VectorInt, 2, false, true}},
        {0x1E, {"V_XNOR_B32", OpClass::VectorInt, 2, false, true}},
        {0x1F, {"V_MAC_F32", OpClass::VectorFloat, 3, true, false}},
        {0x25, {"V_ADD_NC_U32", OpClass::VectorInt, 2, false, true}},
        {0x26, {"V_SUB_NC_U32", OpClass::VectorInt, 2, false, true}},
        {0x27, {"V_SUBREV_NC_U32", OpClass::VectorInt, 2, false, true}},
        {0x2B, {"V_FMAC_F32", OpClass::VectorFloat, 3, true, false}},
    };
    return table;
}

// VOP3 10-bit opcode space: VOP2-class ops live at 0x100 + VOP2 opcode,
// VOPC-class at 0x200 + opcode, VOP1-class at 0x300 + opcode.
const std::unordered_map<uint32_t, OpInfo>& vop3_table() {
    static const std::unordered_map<uint32_t, OpInfo> table = {
        {0x101, {"V_CNDMASK_B32", OpClass::VectorInt, 3, false, false}},
        {0x103, {"V_ADD_F32", OpClass::VectorFloat, 2, true, true}},
        {0x104, {"V_SUB_F32", OpClass::VectorFloat, 2, true, true}},
        {0x105, {"V_SUBREV_F32", OpClass::VectorFloat, 2, true, true}},
        {0x107, {"V_MUL_LEGACY_F32", OpClass::VectorFloat, 2, true, true}},
        {0x108, {"V_MUL_F32", OpClass::VectorFloat, 2, true, true}},
        {0x10F, {"V_MIN_F32", OpClass::VectorFloat, 2, true, true}},
        {0x110, {"V_MAX_F32", OpClass::VectorFloat, 2, true, true}},
        {0x111, {"V_MIN_I32", OpClass::VectorInt, 2, false, true}},
        {0x112, {"V_MAX_I32", OpClass::VectorInt, 2, false, true}},
        {0x113, {"V_MIN_U32", OpClass::VectorInt, 2, false, true}},
        {0x114, {"V_MAX_U32", OpClass::VectorInt, 2, false, true}},
        {0x116, {"V_LSHRREV_B32", OpClass::VectorInt, 2, false, true}},
        {0x118, {"V_ASHRREV_I32", OpClass::VectorInt, 2, false, true}},
        {0x11A, {"V_LSHLREV_B32", OpClass::VectorInt, 2, false, true}},
        {0x11B, {"V_AND_B32", OpClass::VectorInt, 2, false, true}},
        {0x11C, {"V_OR_B32", OpClass::VectorInt, 2, false, true}},
        {0x11D, {"V_XOR_B32", OpClass::VectorInt, 2, false, true}},
        {0x125, {"V_ADD_NC_U32", OpClass::VectorInt, 2, false, true}},
        {0x126, {"V_SUB_NC_U32", OpClass::VectorInt, 2, false, true}},
        {0x127, {"V_SUBREV_NC_U32", OpClass::VectorInt, 2, false, true}},
        {0x140, {"V_MAD_LEGACY_F32", OpClass::VectorFloat, 3, true, false}},
        {0x144, {"V_CUBEID_F32", OpClass::VectorFloat, 3, true, false}},
        {0x14B, {"V_FMA_F32", OpClass::VectorFloat, 3, true, true}},
        {0x151, {"V_MIN3_F32", OpClass::VectorFloat, 3, true, true}},
        {0x154, {"V_MAX3_F32", OpClass::VectorFloat, 3, true, true}},
        {0x157, {"V_MED3_F32", OpClass::VectorFloat, 3, true, false}},
        {0x15F, {"V_DIV_FIXUP_F32", OpClass::VectorFloat, 3, true, false}},
        {0x200, {"V_INTERP_P1_F32", OpClass::VectorFloat, 2, true, false}},
        {0x201, {"V_INTERP_P2_F32", OpClass::VectorFloat, 2, true, false}},
        {0x202, {"V_INTERP_MOV_F32", OpClass::VectorFloat, 2, true, false}},
        {0x360, {"V_READLANE_B32", OpClass::VectorMove, 2, false, false}},
        {0x361, {"V_WRITELANE_B32", OpClass::VectorMove, 2, false, false}},
    };
    return table;
}

} // namespace

// ---------------------------------------------------------------------------

const float* float_const_table() {
    static const float kConsts[8] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};
    return kConsts;
}

Operand decode_scalar_operand(uint32_t raw8) {
    Operand op;
    const uint32_t v = raw8 & 0xFF;
    if (v <= 101) {
        op.kind = Operand::Kind::Sgpr;
        op.index = v;
    } else if (v == 106 || v == 107) {
        op.kind = Operand::Kind::Vcc;
        op.index = v - 106;
    } else if (v == 124) {
        op.kind = Operand::Kind::M0;
    } else if (v == 125) {
        op.kind = Operand::Kind::Null;
    } else if (v == 126 || v == 127) {
        op.kind = Operand::Kind::Exec;
        op.index = v - 126;
    } else if (v >= 128 && v <= 192) {
        op.kind = Operand::Kind::UintConst;
        op.index = v - 128;
    } else if (v >= 193 && v <= 208) {
        op.kind = Operand::Kind::IntConst;
        op.index = v - 192;
    } else if (v >= 240 && v <= 247) {
        op.kind = Operand::Kind::FloatConst;
        op.index = v - 240;
    } else if (v == 255) {
        op.kind = Operand::Kind::Literal;
    } else {
        op.kind = Operand::Kind::Unknown;
    }
    return op;
}

uint32_t format_word_count(Format f) {
    switch (f) {
    case Format::VOP3:
    case Format::EXP:
        return 2;
    case Format::Unknown:
        return 0;
    default:
        return 1;
    }
}

bool format_is_8_byte(Format f) { return format_word_count(f) == 2; }

const char* format_name(Format f) {
    switch (f) {
    case Format::SOP1: return "SOP1";
    case Format::SOP2: return "SOP2";
    case Format::SOPK: return "SOPK";
    case Format::SOPC: return "SOPC";
    case Format::SOPP: return "SOPP";
    case Format::VOP1: return "VOP1";
    case Format::VOP2: return "VOP2";
    case Format::VOP3: return "VOP3";
    case Format::VOPC: return "VOPC";
    case Format::EXP: return "EXP";
    case Format::DS: return "DS";
    case Format::FLAT: return "FLAT";
    case Format::MUBUF: return "MUBUF";
    case Format::MTBUF: return "MTBUF";
    case Format::MIMG: return "MIMG";
    case Format::SMEM: return "SMEM";
    case Format::Unknown: return "Unknown";
    }
    return "?";
}

const OpInfo* lookup_op(Format format, uint32_t opcode) {
    const std::unordered_map<uint32_t, OpInfo>* table = nullptr;
    switch (format) {
    case Format::SOP1: table = &sop1_table(); break;
    case Format::SOP2: table = &sop2_table(); break;
    case Format::SOPK: table = &sopk_table(); break;
    case Format::SOPP: table = &sopp_table(); break;
    case Format::VOP1: table = &vop1_table(); break;
    case Format::VOP2: table = &vop2_table(); break;
    case Format::VOP3: table = &vop3_table(); break;
    default: return nullptr;
    }
    auto it = table->find(opcode);
    if (it == table->end()) return nullptr;
    return &it->second;
}

std::expected<Instr, Error> decode(std::span<const uint32_t> words) {
    if (words.empty()) {
        return std::unexpected(Error{Status::InvalidArgument, "empty instruction stream"});
    }
    const uint32_t w0 = words[0];
    const Family fam = classify_word0(w0);
    Instr out;
    out.format = fam.format;
    if (!fam.ok) {
        return std::unexpected(
            Error{Status::Unsupported, "reserved RDNA2 encoding"});
    }
    if (words.size() < format_word_count(fam.format)) {
        return std::unexpected(
            Error{Status::InvalidArgument, "truncated RDNA2 instruction"});
    }

    switch (fam.format) {
    case Format::VOP2: {
        // [30:25] op, [24:17] vdst, [16:9] src1 (VGPR), [8:0] src0 (9-bit)
        out.opcode = (w0 >> 25) & 0x3F;
        out.dst = (w0 >> 17) & 0xFF;
        out.src1 = (w0 >> 9) & 0xFF;
        out.src1_is_vgpr = true; // GFX10 e32 src1 is VGPR-only
        out.src0 = w0 & 0x1FF;
        out.src0_is_vgpr = (out.src0 >> 8) & 1;
        if ((out.src0 & 0xFF) == 255) {
            out.literal_src = !out.src0_is_vgpr;
            if (out.literal_src) {
                if (words.size() < 2) {
                    return std::unexpected(
                        Error{Status::InvalidArgument, "truncated literal"});
                }
                out.two_word = true;
                out.imm32 = words[1];
            }
        }
        return out;
    }
    case Format::VOP1: {
        // [31:25] = 0x3F, [24:17] vdst, [16:9] op, [8:0] src0
        out.opcode = (w0 >> 9) & 0xFF;
        out.dst = (w0 >> 17) & 0xFF;
        out.src0 = w0 & 0x1FF;
        out.src0_is_vgpr = (out.src0 >> 8) & 1;
        if ((out.src0 & 0xFF) == 255) {
            out.literal_src = !out.src0_is_vgpr;
            if (out.literal_src) {
                if (words.size() < 2) {
                    return std::unexpected(
                        Error{Status::InvalidArgument, "truncated literal"});
                }
                out.two_word = true;
                out.imm32 = words[1];
            }
        }
        return out;
    }
    case Format::VOPC: {
        out.opcode = (w0 >> 17) & 0xFF;
        out.src0 = w0 & 0x1FF;
        out.src0_is_vgpr = (out.src0 >> 8) & 1;
        return out;
    }
    case Format::VOP3: {
        // word0: [25:16] op (10 bits), [15] clamp, [7:0] vdst
        // word1: [8:0] src0, [17:9] src1, [26:18] src2, [28:27] omod
        const uint32_t w1 = words[1];
        out.opcode = (w0 >> 16) & 0x3FF;
        out.dst = w0 & 0xFF;
        out.clamp = (w0 >> 15) & 1;
        out.src0 = w1 & 0x1FF;
        out.src1 = (w1 >> 9) & 0x1FF;
        out.src2 = (w1 >> 18) & 0x1FF;
        out.omod = (w1 >> 27) & 0x3;
        out.src0_is_vgpr = (out.src0 >> 8) & 1;
        out.src1_is_vgpr = (out.src1 >> 8) & 1;
        out.src2_is_vgpr = (out.src2 >> 8) & 1;
        out.two_word = true;
        out.literal_src = (out.src0 & 0xFF) == 255 && !out.src0_is_vgpr;
        if (out.literal_src) {
            if (words.size() < 3) {
                return std::unexpected(
                    Error{Status::InvalidArgument, "truncated literal"});
            }
            out.imm32 = words[2];
        }
        return out;
    }
    case Format::EXP: {
        // word0: [3:0] en, [9:4] target, [10] compr, [11] done, [12] vm
        // word1: [7:0] src0, [15:8] src1, [23:16] src2, [31:24] src3
        const uint32_t w1 = words[1];
        out.en = w0 & 0xF;
        out.target = (w0 >> 4) & 0x3F;
        out.compr = (w0 >> 10) & 1;
        out.done = (w0 >> 11) & 1;
        out.vm = (w0 >> 12) & 1;
        out.src0 = w1 & 0xFF;
        out.src1 = (w1 >> 8) & 0xFF;
        out.src2 = (w1 >> 16) & 0xFF;
        out.dst = (w1 >> 24) & 0xFF; // src3 reuses dst slot (4th export src)
        out.two_word = true;
        return out;
    }
    case Format::SOP1: {
        // [31:23] = 0x17D, [22:16] sdst, [15:8] op, [7:0] src0
        out.opcode = (w0 >> 8) & 0xFF;
        out.dst = (w0 >> 16) & 0x7F;
        out.src0 = w0 & 0xFF;
        out.literal_src = (out.src0 == 255);
        if (out.literal_src) {
            if (words.size() < 2) {
                return std::unexpected(
                    Error{Status::InvalidArgument, "truncated literal"});
            }
            out.two_word = true;
            out.imm32 = words[1];
        }
        return out;
    }
    case Format::SOP2: {
        // [31:30] = 0b10, [29:23] op, [22:16] sdst, [15:8] src1, [7:0] src0
        out.opcode = (w0 >> 23) & 0x7F;
        out.dst = (w0 >> 16) & 0x7F;
        out.src1 = (w0 >> 8) & 0xFF;
        out.src0 = w0 & 0xFF;
        out.literal_src = (out.src0 == 255 || out.src1 == 255);
        if (out.literal_src) {
            if (words.size() < 2) {
                return std::unexpected(
                    Error{Status::InvalidArgument, "truncated literal"});
            }
            out.two_word = true;
            out.imm32 = words[1];
        }
        return out;
    }
    case Format::SOPK: {
        // [31:28] = 0b1011, [27:23] op, [22:16] sdst, [15:0] simm16
        out.opcode = (w0 >> 23) & 0x1F;
        out.dst = (w0 >> 16) & 0x7F;
        out.imm16 = w0 & 0xFFFF;
        return out;
    }
    case Format::SOPC: {
        // [31:23] = 0x17E, [22:16] op, [15:8] src1, [7:0] src0
        out.opcode = (w0 >> 16) & 0x7F;
        out.src1 = (w0 >> 8) & 0xFF;
        out.src0 = w0 & 0xFF;
        out.literal_src = (out.src0 == 255 || out.src1 == 255);
        return out;
    }
    case Format::SOPP: {
        // [31:23] = 0x17F, [22:16] op, [15:0] simm16
        out.opcode = (w0 >> 16) & 0x7F;
        out.imm16 = w0 & 0xFFFF;
        return out;
    }
    case Format::DS:
    case Format::FLAT:
    case Format::MUBUF:
    case Format::MTBUF:
    case Format::MIMG:
    case Format::SMEM:
        // Classified only; field layout pending verification.
        out.opcode = 0;
        return out;
    default:
        break;
    }
    return std::unexpected(Error{Status::Unsupported, "unhandled RDNA2 family"});
}

std::expected<Instr, Error> decode(uint32_t word) {
    return decode(std::span<const uint32_t>{&word, 1});
}

} // namespace armsx5::gpu::rdna2
