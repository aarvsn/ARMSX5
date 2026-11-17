#include "armsx5/cpu/x86/decoder.h"

#include "armsx5/cpu/cpu_state.h"

#include <expected>
#include <cstring>
#include <string>

namespace armsx5::cpu::x86 {
namespace {

constexpr uint8_t kNoReg = 0xFF;

struct Prefixes {
    bool opsize16 = false;
    bool rep = false;        // F3
    bool repne = false;      // F2
    bool lock = false;       // F0 (accepted, ignored: guest is single-threaded HLE)
    bool seg_cs = false, seg_ds = false, seg_es = false, seg_ss = false, seg_fs = false,
         seg_gs = false;
    bool rex_present = false;
    bool rex_w = false, rex_r = false, rex_x = false, rex_b = false;

    // VEX (AVX) fields, valid when vex == true. VEX is the LAST prefix:
    // after its payload the opcode byte follows immediately.
    bool vex = false;
    bool vex_w = false;   // VEX.W (byte2 bit7): 64-bit GPR operand
    bool vex_l = false;   // VEX.L (byte2 bit2): 256-bit vector length
    uint8_t vex_pp = 0;   // VEX.pp (byte2 bits1:0): 0=none 1=66 2=F3 3=F2
    uint8_t vex_mmmmm = 1;// opcode map: 1 = 0F, 2 = 0F38, 3 = 0F3A
    uint8_t vex_v_raw = 15; // raw (1's-complement) vvvv nibble
    bool vex_r = true, vex_x = true, vex_b = true; // inverted extension bits
    uint8_t vvvv() const { return static_cast<uint8_t>(~vex_v_raw & 0xF); }
};

// Reads a little-endian integer of `bytes` length.
int64_t read_le(const uint8_t* p, size_t bytes) {
    uint64_t v = 0;
    for (size_t i = 0; i < bytes; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return static_cast<int64_t>(v);
}

int64_t sign_extend(uint64_t v, size_t bits) {
    const uint64_t sign = 1ULL << (bits - 1);
    if (v & sign) v |= ~((1ULL << bits) - 1);
    return static_cast<int64_t>(v);
}

struct Decoder {
    const uint8_t* data;
    size_t max_len;
    size_t pos = 0;
    Prefixes pre{};
    uint8_t opsize = 4;

    bool take(uint8_t& b) {
        if (pos >= max_len) return false;
        b = data[pos++];
        return true;
    }
    bool peek(uint8_t& b) const {
        if (pos >= max_len) return false;
        b = data[pos];
        return true;
    }

    bool parse_prefixes() {
        for (int i = 0; i < 15; ++i) {
            uint8_t b;
            if (!peek(b)) return false;
            switch (b) {
            case 0xF0: pre.lock = true; ++pos; continue;
            case 0xF2: pre.repne = true; ++pos; continue;
            case 0xF3: pre.rep = true; ++pos; continue;
            case 0x66: pre.opsize16 = true; ++pos; continue;
            case 0x67: return false; // 16-bit addressing unsupported in long mode subset
            case 0x2E: pre.seg_cs = true; ++pos; continue;
            case 0x3E: pre.seg_ds = true; ++pos; continue;
            case 0x26: pre.seg_es = true; ++pos; continue;
            case 0x36: pre.seg_ss = true; ++pos; continue;
            case 0x64: pre.seg_fs = true; ++pos; continue;
            case 0x65: pre.seg_gs = true; ++pos; continue;
            case 0xC4: case 0xC5: { // VEX.3-byte / VEX.2-byte (long mode)
                // Legacy LES/LDS opcodes are invalid in long mode, so these
                // bytes always introduce a VEX prefix. It terminates prefix
                // processing: the opcode byte follows the VEX payload.
                ++pos;
                pre.vex = true;
                pre.rex_present = false; // a REX before VEX would be #UD; treat as absent
                uint8_t b1;
                if (!take(b1)) return false;
                if (b == 0xC5) {
                    pre.vex_r = (b1 & 0x80) != 0;
                    pre.vex_v_raw = (b1 >> 3) & 0xF;
                    pre.vex_l = (b1 & 0x4) != 0;
                    pre.vex_pp = b1 & 0x3;
                    pre.vex_x = pre.vex_b = true; // unused (reserved-1)
                    pre.vex_w = false;
                    pre.vex_mmmmm = 1; // 0F map
                } else {
                    pre.vex_r = (b1 & 0x80) != 0;
                    pre.vex_x = (b1 & 0x40) != 0;
                    pre.vex_b = (b1 & 0x20) != 0;
                    pre.vex_mmmmm = b1 & 0x1F;
                    uint8_t b2;
                    if (!take(b2)) return false;
                    pre.vex_w = (b2 & 0x80) != 0;
                    pre.vex_v_raw = (b2 >> 3) & 0xF;
                    pre.vex_l = (b2 & 0x4) != 0;
                    pre.vex_pp = b2 & 0x3;
                }
                if (pre.vex_mmmmm == 0 || pre.vex_mmmmm > 3)
                    return false; // reserved opcode maps are invalid prefixes
                return true;
            }
            default:
                if (b >= 0x40 && b <= 0x4F) { // REX
                    pre.rex_present = true;
                    pre.rex_w = (b & 0x8) != 0;
                    pre.rex_r = (b & 0x4) != 0;
                    pre.rex_x = (b & 0x2) != 0;
                    pre.rex_b = (b & 0x1) != 0;
                    ++pos;
                    continue;
                }
                return true;
            }
        }
        return false; // exceeded prefix budget
    }

    uint8_t eff_opsize(bool byte = false) const {
        if (byte) return 1;
        if (pre.rex_w) return 8;
        if (pre.opsize16) return 2;
        return 4;
    }

    // Decodes the ModRM/SIB/displacement sequence into `rm` and returns the
    // reg-field index (REX.R applied).
    std::expected<Operand, Error> decode_modrm(Operand& rm, uint8_t& reg_field) {
        uint8_t b;
        if (!take(b)) return std::unexpected(Error{Status::ParseError, "truncated ModRM"});
        const uint8_t mod = b >> 6;
        uint8_t reg_idx = ((b >> 3) & 7) + (pre.rex_r ? 8 : 0);
        uint8_t rm_idx = (b & 7) + (pre.rex_b ? 8 : 0);
        reg_field = reg_idx;

        if (mod == 3) {
            rm.kind = OperandKind::Reg;
            rm.reg = rm_reg_index(rm_idx);
            rm.high_byte = !pre.rex_present && rm_idx >= 4 && rm_idx < 8 && opsize == 1;
            rm.size = opsize;
            return Operand{};
        }

        MemOperand m;
        m.valid = true;
        if (rm_idx == 4) { // SIB byte follows (raw rm==4, before REX.B extension)
            uint8_t sib;
            if (!take(sib)) return std::unexpected(Error{Status::ParseError, "truncated SIB"});
            const uint8_t scale_bits = sib >> 6;
            const uint8_t idx_bits = (sib >> 3) & 7;
            const uint8_t base_bits = sib & 7;
            m.scale = 1u << scale_bits;
            if (idx_bits != 4) m.index = idx_bits + (pre.rex_x ? 8 : 0);
            if (base_bits == 5 && mod == 0) {
                // base absent, disp32 (plus optional index)
            } else {
                m.base = base_bits + (pre.rex_b ? 8 : 0);
            }
        } else if (rm_idx == 5 && mod == 0) {
            m.rip_relative = true;
        } else {
            m.base = rm_idx;
        }

        if (mod == 1) {
            uint8_t d8;
            if (!take(d8)) return std::unexpected(Error{Status::ParseError, "truncated disp8"});
            m.disp = sign_extend(d8, 8);
        } else if (mod == 2) {
            if (pos + 4 > max_len)
                return std::unexpected(Error{Status::ParseError, "truncated disp32"});
            m.disp = sign_extend(static_cast<uint64_t>(read_le(data + pos, 4)), 32);
            pos += 4;
        } else if ((rm_idx & 7) == 5 && mod == 0) {
            if (pos + 4 > max_len)
                return std::unexpected(Error{Status::ParseError, "truncated disp32"});
            m.disp = sign_extend(static_cast<uint64_t>(read_le(data + pos, 4)), 32);
            pos += 4;
        }
        rm.kind = OperandKind::Mem;
        rm.mem = m;
        rm.size = opsize;
        return Operand{};
    }

    uint8_t rm_reg_index(uint8_t raw_rm) const {
        // For mod==3: 8-bit ops without REX map 4..7 to high bytes of 0..3.
        if (opsize == 1 && !pre.rex_present) {
            if (raw_rm >= 4 && raw_rm < 8) return raw_rm - 4; // AH..BH on AX..BX
            return raw_rm; // 0..7 as AL..R15B (with REX.B)
        }
        return raw_rm;
    }

    std::expected<int64_t, Error> read_imm(size_t bytes, bool sign_ext) {
        if (pos + bytes > max_len)
            return std::unexpected(Error{Status::ParseError, "truncated immediate"});
        uint64_t v = static_cast<uint64_t>(read_le(data + pos, bytes));
        pos += bytes;
        if (sign_ext && bytes < 8) return sign_extend(v, bytes * 8);
        return static_cast<int64_t>(v);
    }
};

} // namespace

// AVX (VEX) opcode decoding. Called after VEX prefix normalization with the
// opcode byte in hand. Three-operand AVX layout: dst = modrm.reg, src1 =
// VEX.vvvv (kept in out.vvvv), src2 = modrm.rm (reg or memory).
std::expected<Decoded, Error> decode_avx(Decoder& d, uint8_t op2, Decoded& out) {
    if (d.pre.vex_mmmmm != 1)
        return std::unexpected(
            Error{Status::Unsupported, "VEX opcode map 0F38/0F3A not implemented"});
    out.vex = true;
    out.vex_l = d.pre.vex_l;
    out.vvvv = d.pre.vvvv();
    out.opsize = d.pre.vex_l ? 32 : 16;
    d.opsize = d.pre.vex_l ? 32 : 16;

    // Vector destination = reg field; source = rm (reg or memory).
    auto vec_modrm = [&](Operand& dst, Operand& src2) -> std::expected<void, Error> {
        uint8_t regf;
        auto rd = d.decode_modrm(src2, regf);
        if (!rd) return std::unexpected(rd.error());
        dst.kind = OperandKind::Reg;
        dst.reg = regf;
        dst.size = d.opsize;
        if (src2.kind == OperandKind::Reg) src2.size = d.opsize;
        return {};
    };
    using F = SseForm;
    auto reject = [](const char* what) {
        char msg[96];
        snprintf(msg, sizeof(msg), "AVX %s not implemented", what);
        return std::unexpected(Error{Status::Unsupported, msg});
    };
    // VEX.pp replaces the legacy prefix bytes: 0 = none (PS), 1 = 66 (PD),
    // 2 = F3 (SS), 3 = F2 (SD).
    auto float_form = [&]() {
        return d.pre.vex_pp == 2 ? F::SS
               : d.pre.vex_pp == 3 ? F::SD
                                   : d.pre.vex_pp == 1 ? F::PD : F::PS;
    };

    switch (op2) {
    case 0x10: case 0x11: { // VMOVUPS/VMOVSS/VMOVSD/VMOVUPD (+stores)
        const bool store = op2 == 0x11;
        out.cls = InstrClass::SseMov;
        out.sse_form = float_form();
        auto r = vec_modrm(out.dst, out.src);
        if (!r) return std::unexpected(r.error());
        if (store) {
            std::swap(out.dst, out.src); // reg-field -> rm on stores
            if (out.sse_form == F::SS) out.dst.size = out.src.size = 4;
            else if (out.sse_form == F::SD) out.dst.size = out.src.size = 8;
        } else {
            if (out.sse_form == F::SS) out.src.size = out.dst.size = 4;
            else if (out.sse_form == F::SD) out.src.size = out.dst.size = 8;
        }
        break;
    }
    case 0x28: case 0x29: { // VMOVAPS/VMOVAPD (aligned)
        const bool store = op2 == 0x29;
        out.cls = InstrClass::SseMov;
        out.sse_form = F::AP;
        auto r = vec_modrm(out.dst, out.src);
        if (!r) return std::unexpected(r.error());
        if (store) std::swap(out.dst, out.src);
        break;
    }
    case 0x2A: { // VCVTSI2SS (F3) / VCVTSI2SD (F2); W selects the GPR size
        if (d.pre.vex_pp != 2 && d.pre.vex_pp != 3) return reject("VCVTSI2PS (MMX)");
        out.cls = InstrClass::SseCvt;
        out.sse_op = d.pre.vex_pp == 2 ? SseOp::IntToF32 : SseOp::IntToF64;
        out.sse_form = d.pre.vex_pp == 2 ? F::SS : F::SD;
        uint8_t regf;
        auto rd = d.decode_modrm(out.src, regf); // src2 = rm (GPR/mem)
        if (!rd) return std::unexpected(rd.error());
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = regf;
        out.dst.size = 16;
        out.src.size = d.pre.vex_w ? 8 : 4;
        break;
    }
    case 0x2C: case 0x2D: { // VCVTTSS2SI/VCVTSS2SI (+SD forms); W selects dst size
        if (d.pre.vex_pp != 2 && d.pre.vex_pp != 3) return reject("CVTPI2PI (MMX)");
        out.cls = InstrClass::SseCvt;
        const bool trunc = op2 == 0x2C;
        const bool f64 = d.pre.vex_pp == 3;
        out.sse_op = trunc ? (f64 ? SseOp::TruncF64ToI : SseOp::TruncF32ToI)
                           : (f64 ? SseOp::RoundF64ToI : SseOp::RoundF32ToI);
        out.sse_form = f64 ? F::SD : F::SS;
        uint8_t regf;
        auto rd = d.decode_modrm(out.src, regf); // src = vector reg/mem
        if (!rd) return std::unexpected(rd.error());
        if (out.src.kind == OperandKind::Reg) out.src.size = 16;
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = regf;
        out.dst.size = d.pre.vex_w ? 8 : 4;
        break;
    }
    case 0x2E: case 0x2F: { // VUCOMISS/VCOMISS (+SD forms)
        out.cls = InstrClass::SseComis;
        out.cond = op2 == 0x2F ? 1 : 0; // 1 = COMIS (signals QNaN), 0 = UCOMIS
        out.sse_form = d.pre.vex_pp == 1 ? F::SD : F::SS;
        auto r = vec_modrm(out.dst, out.src);
        if (!r) return std::unexpected(r.error());
        if (out.src.kind == OperandKind::Reg)
            out.src.size = out.sse_form == F::SD ? 8 : 4;
        break;
    }
    case 0x51: { // VSQRTSS/VSQRTSD/VSQRTPS/VSQRTPD
        out.cls = InstrClass::SseArith;
        out.sse_op = SseOp::Sqrt;
        out.sse_form = float_form();
        auto r = vec_modrm(out.dst, out.src);
        if (!r) return std::unexpected(r.error());
        break;
    }
    case 0x54: case 0x55: case 0x56: case 0x57: { // VAND/VANDN/VOR/VXOR PS/PD
        out.cls = InstrClass::SseArith;
        static const SseOp kOps[4] = {SseOp::And, SseOp::Andn, SseOp::Or, SseOp::Xor};
        out.sse_op = kOps[op2 - 0x54];
        out.sse_form = d.pre.vex_pp == 1 ? F::PD : F::PS;
        auto r = vec_modrm(out.dst, out.src);
        if (!r) return std::unexpected(r.error());
        break;
    }
    case 0x58: case 0x59: case 0x5C: case 0x5E: { // VADD/VMUL/VSUB/VDIV
        out.cls = InstrClass::SseArith;
        static const SseOp kOps[4] = {SseOp::Add, SseOp::Mul, SseOp::Sub, SseOp::Div};
        SseOp base = op2 == 0x58 ? SseOp::Add : op2 == 0x59 ? SseOp::Mul
                     : op2 == 0x5C ? SseOp::Sub : SseOp::Div;
        out.sse_op = base;
        out.sse_form = float_form();
        auto r = vec_modrm(out.dst, out.src);
        if (!r) return std::unexpected(r.error());
        break;
    }
    case 0x5B: { // VCVTDQ2PS / VCVTPS2DQ / VCVTTPS2DQ
        if (d.pre.vex_pp == 3) return reject("VCVTPS2PI (MMX)");
        out.cls = InstrClass::SseCvt;
        out.sse_op = d.pre.vex_pp == 1 ? SseOp::F32ToI32x4
                     : d.pre.vex_pp == 2 ? SseOp::TruncF32ToI32x4
                                         : SseOp::I32ToF32x4;
        out.sse_form = F::PS;
        auto r = vec_modrm(out.dst, out.src);
        if (!r) return std::unexpected(r.error());
        break;
    }
    case 0x6E: { // VMOVD/VMOVQ GPR/mem -> xmm (66; W selects GPR size)
        if (d.pre.vex_pp != 1) return reject("MOVD (MMX)");
        out.cls = InstrClass::SseMov;
        out.sse_form = d.pre.vex_w ? F::Q : F::D;
        uint8_t regf;
        auto rd = d.decode_modrm(out.src, regf);
        if (!rd) return std::unexpected(rd.error());
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = regf;
        out.dst.size = 16;
        out.src.size = d.pre.vex_w ? 8 : 4;
        break;
    }
    case 0x6F: { // VMOVDQA (66) / VMOVDQU (F3)
        if (d.pre.vex_pp != 1 && d.pre.vex_pp != 2) return reject("MOVQ (MMX)");
        out.cls = InstrClass::SseMov;
        out.sse_form = d.pre.vex_pp == 1 ? F::DQA : F::DQU;
        auto r = vec_modrm(out.dst, out.src);
        if (!r) return std::unexpected(r.error());
        break;
    }
    case 0x70: { // VPSHUFD (66)
        if (d.pre.vex_pp != 1) return reject("PSHUFW/L/H");
        out.cls = InstrClass::SseShuffle;
        out.sse_form = F::DQA;
        auto r = vec_modrm(out.dst, out.src);
        if (!r) return std::unexpected(r.error());
        auto imm = d.read_imm(1, false);
        if (!imm) return std::unexpected(imm.error());
        out.imm3 = *imm;
        break;
    }
    case 0x7E: { // VMOVD/VMOVQ xmm -> GPR/mem (66), VMOVQ xmm, xmm/m64 (F3)
        if (d.pre.vex_pp != 1 && d.pre.vex_pp != 2) return reject("MOVQ (MMX)");
        out.cls = InstrClass::SseMov;
        uint8_t regf;
        auto rd = d.decode_modrm(out.dst, regf);
        if (!rd) return std::unexpected(rd.error());
        out.src.kind = OperandKind::Reg;
        out.src.reg = regf;
        out.src.size = 16;
        if (d.pre.vex_pp == 2) { // F3 7E: VMOVQ (zero-extends)
            out.sse_form = F::QF3;
            out.dst.size = 16;
        } else {
            out.sse_form = d.pre.vex_w ? F::Q : F::D;
            out.dst.size = d.pre.vex_w ? 8 : 4;
        }
        break;
    }
    case 0x7F: { // VMOVDQA/VMOVDQU store
        if (d.pre.vex_pp != 1 && d.pre.vex_pp != 2) return reject("MOVQ (MMX)");
        out.cls = InstrClass::SseMov;
        out.sse_form = d.pre.vex_pp == 1 ? F::DQA : F::DQU;
        auto r = vec_modrm(out.dst, out.src);
        if (!r) return std::unexpected(r.error());
        std::swap(out.dst, out.src); // reg-field -> rm on stores
        break;
    }
    case 0xC2: { // VCMPSS/VCMPPS/VCMPSD/VCMPPD + imm8 predicate
        out.cls = InstrClass::SseCmp;
        out.sse_form = float_form();
        auto r = vec_modrm(out.dst, out.src);
        if (!r) return std::unexpected(r.error());
        auto imm = d.read_imm(1, false);
        if (!imm) return std::unexpected(imm.error());
        if (*imm < 0 || *imm > 7)
            return std::unexpected(Error{Status::Unsupported, "AVX CMP predicate (>7)"});
        out.sse_pred = static_cast<uint8_t>(*imm);
        break;
    }
    case 0xD4: case 0xD5: case 0xD6: case 0xF4:
    case 0xF8: case 0xF9: case 0xFA: case 0xFB:
    case 0xFC: case 0xFD: case 0xFE: { // VPADD*/VPSUB*/VPMULLW/VPMULUDQ (66)
        if (d.pre.vex_pp != 1) return reject("packed integer (MMX)");
        out.cls = InstrClass::SseInt;
        switch (op2) {
        case 0xD4: out.sse_op = SseOp::PaddQ; break;
        case 0xD5: out.sse_op = SseOp::Pmullw; break;
        case 0xD6: { // VMOVQ xmm/m64, xmm (store low 64; rm dst keeps upper
                     // 64 bits of its low half when a register)
            out.cls = InstrClass::SseMov;
            out.sse_form = F::QD6;
            auto r = vec_modrm(out.dst, out.src);
            if (!r) return std::unexpected(r.error());
            std::swap(out.dst, out.src); // reg-field -> rm on stores
            out.dst.size = out.src.size = 8;
            break;
        }
        case 0xF4: out.sse_op = SseOp::Pmuludq; break;
        case 0xF8: out.sse_op = SseOp::PsubB; break;
        case 0xF9: out.sse_op = SseOp::PsubW; break;
        case 0xFA: out.sse_op = SseOp::PsubD; break;
        case 0xFB: out.sse_op = SseOp::PsubQ; break;
        case 0xFC: out.sse_op = SseOp::PaddB; break;
        case 0xFD: out.sse_op = SseOp::PaddW; break;
        case 0xFE: out.sse_op = SseOp::PaddD; break;
        }
        if (out.cls == InstrClass::SseInt) {
            auto r = vec_modrm(out.dst, out.src);
            if (!r) return std::unexpected(r.error());
        }
        break;
    }
    default: {
        char msg[72];
        snprintf(msg, sizeof(msg), "AVX 0F %02X (VEX map 0F) not implemented", op2);
        return std::unexpected(Error{Status::Unsupported, msg});
    }
    }

    out.length = static_cast<uint8_t>(d.pos);
    if (out.length == 0)
        return std::unexpected(Error{Status::ParseError, "zero-length instruction"});
    return out;
}

std::expected<Decoded, Error> decode(const uint8_t* code, size_t max_len) {
    if (!code || max_len == 0)
        return std::unexpected(Error{Status::InvalidArgument, "no code bytes"});

    Decoder d{code, max_len};
    if (!d.parse_prefixes())
        return std::unexpected(Error{Status::ParseError, "prefix stream invalid or too long"});

    // ---- VEX (AVX) path ----
    // VEX.R/X/B carry the same meaning as REX.R/X/B for ModRM decoding but
    // inverted (0 = extended). Normalize so decode_modrm applies them
    // uniformly; VEX.W replaces REX.W.
    if (d.pre.vex) {
        d.pre.rex_present = true;
        d.pre.rex_w = d.pre.vex_w;
        d.pre.rex_r = !d.pre.vex_r;
        d.pre.rex_x = !d.pre.vex_x;
        d.pre.rex_b = !d.pre.vex_b;
        uint8_t op2;
        if (!d.take(op2))
            return std::unexpected(Error{Status::ParseError, "truncated VEX opcode"});
        Decoded out;
        return decode_avx(d, op2, out);
    }

    uint8_t op;
    if (!d.take(op)) return std::unexpected(Error{Status::ParseError, "missing opcode"});

    Decoded out;
    out.opsize = d.opsize;

    // ---- single-byte opcodes ----
    // ALU block 0x00-0x3F uses forms 0-5; forms 6/7 (0x06/0x07/0x0E/0x0F...)
    // are push/pop segment prefixes and the two-byte opcode escape.
    if (op < 0x40 && (op & 7) < 6) { // ALU block: ADD OR ADC SBB AND SUB XOR CMP
        const uint8_t alu = (op >> 3) & 7;
        const uint8_t form = op & 7;
        static const AluOp kAlu[8] = {AluOp::Add, AluOp::Or, AluOp::Adc, AluOp::Sbb,
                                      AluOp::And, AluOp::Sub, AluOp::Xor, AluOp::Cmp};
        out.alu_op = kAlu[alu];
        out.cls = InstrClass::Alu;
        switch (form) {
        case 0: { // r/m8, r8
            d.opsize = 1;
            out.opsize = 1;
            uint8_t regf;
            auto rd = d.decode_modrm(out.dst, regf);
            if (!rd) return std::unexpected(rd.error());
            out.src.kind = OperandKind::Reg;
            out.src.reg = regf;
            out.src.high_byte = !d.pre.rex_present && regf >= 4 && regf < 8;
            out.src.size = 1;
            break;
        }
        case 1: { // r/m, r
            uint8_t regf;
            auto rd = d.decode_modrm(out.dst, regf);
            if (!rd) return std::unexpected(rd.error());
            out.opsize = d.eff_opsize();
            out.dst.size = out.opsize;
            out.src.kind = OperandKind::Reg;
            out.src.reg = regf;
            out.src.size = out.opsize;
            break;
        }
        case 2: { // r8, r/m8
            d.opsize = 1;
            out.opsize = 1;
            uint8_t regf;
            auto rd = d.decode_modrm(out.src, regf);
            if (!rd) return std::unexpected(rd.error());
            out.dst.kind = OperandKind::Reg;
            out.dst.reg = regf;
            out.dst.high_byte = !d.pre.rex_present && regf >= 4 && regf < 8;
            out.dst.size = 1;
            break;
        }
        case 3: { // r, r/m
            uint8_t regf;
            auto rd = d.decode_modrm(out.src, regf);
            if (!rd) return std::unexpected(rd.error());
            out.opsize = d.eff_opsize();
            out.dst.kind = OperandKind::Reg;
            out.dst.reg = regf;
            out.dst.size = out.opsize;
            break;
        }
        case 4: { // AL, imm8
            d.opsize = 1;
            out.opsize = 1;
            out.dst.kind = OperandKind::Reg;
            out.dst.reg = reg::AX;
            out.dst.size = 1;
            auto imm = d.read_imm(1, false);
            if (!imm) return std::unexpected(imm.error());
            out.src.kind = OperandKind::Imm;
            out.src.imm = *imm;
            out.src.size = 1;
            break;
        }
        case 5: { // rAX/eAX, imm
            out.opsize = d.eff_opsize();
            out.dst.kind = OperandKind::Reg;
            out.dst.reg = reg::AX;
            out.dst.size = out.opsize;
            const size_t bytes = out.opsize == 2 ? 2 : 4;
            auto imm = d.read_imm(bytes, out.opsize != 2);
            if (!imm) return std::unexpected(imm.error());
            out.src.kind = OperandKind::Imm;
            out.src.imm = *imm;
            out.src.size = out.opsize;
            break;
        }
        default:
            return std::unexpected(Error{Status::ParseError, "invalid ALU opcode form"});
        }
        out.length = static_cast<uint8_t>(d.pos);
        return out;
    }

    switch (op) {
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57: { // PUSH r64
        out.cls = InstrClass::Push;
        out.opsize = 8;
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = (op & 7) + (d.pre.rex_b ? 8 : 0);
        out.dst.size = 8;
        break;
    }
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: { // POP r64
        out.cls = InstrClass::Pop;
        out.opsize = 8;
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = (op & 7) + (d.pre.rex_b ? 8 : 0);
        out.dst.size = 8;
        break;
    }
    case 0x63: { // MOVSXD r32, r/m32 (64-bit with REX.W)
        uint8_t regf;
        auto rd = d.decode_modrm(out.src, regf);
        if (!rd) return std::unexpected(rd.error());
        out.cls = InstrClass::Movsxd;
        out.opsize = d.pre.rex_w ? 8 : 4;
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = regf;
        out.dst.size = out.opsize;
        break;
    }
    case 0x68: { // PUSH imm32 sign-extended to 64 (imm16 with 66)
        out.cls = InstrClass::Push;
        out.opsize = d.pre.opsize16 ? 2 : 8;
        auto imm = d.read_imm(2, true);
        if (!imm) return std::unexpected(imm.error());
        out.src.kind = OperandKind::Imm;
        out.src.imm = *imm;
        out.src.size = out.opsize;
        break;
    }
    case 0x6A: { // PUSH imm8
        out.cls = InstrClass::Push;
        out.opsize = 8;
        auto imm = d.read_imm(1, true);
        if (!imm) return std::unexpected(imm.error());
        out.src.kind = OperandKind::Imm;
        out.src.imm = *imm;
        out.src.size = 8;
        break;
    }
    case 0x69: { // IMUL r, r/m, imm32
        uint8_t regf;
        auto rd = d.decode_modrm(out.src, regf);
        if (!rd) return std::unexpected(rd.error());
        auto imm = d.read_imm(4, true);
        if (!imm) return std::unexpected(imm.error());
        out.cls = InstrClass::IMul;
        out.imul_form = 2;
        out.opsize = d.eff_opsize();
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = regf;
        out.dst.size = out.opsize;
        out.src.size = out.opsize;
        out.imm3 = *imm;
        break;
    }
    case 0x6B: { // IMUL r, r/m, imm8
        uint8_t regf;
        auto rd = d.decode_modrm(out.src, regf);
        if (!rd) return std::unexpected(rd.error());
        auto imm = d.read_imm(1, true);
        if (!imm) return std::unexpected(imm.error());
        out.cls = InstrClass::IMul;
        out.imul_form = 2;
        out.opsize = d.eff_opsize();
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = regf;
        out.dst.size = out.opsize;
        out.src.size = out.opsize;
        out.imm3 = *imm;
        break;
    }
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: { // Jcc rel8
        out.cls = InstrClass::Jcc;
        out.cond = op & 15;
        auto rel = d.read_imm(1, true);
        if (!rel) return std::unexpected(rel.error());
        out.rel = *rel;
        break;
    }
    case 0x80: case 0x81: case 0x83: { // GRP1 r/m, imm
        uint8_t regf;
        auto rd = d.decode_modrm(out.dst, regf);
        if (!rd) return std::unexpected(rd.error());
        static const AluOp kAlu[8] = {AluOp::Add, AluOp::Or, AluOp::Adc, AluOp::Sbb,
                                      AluOp::And, AluOp::Sub, AluOp::Xor, AluOp::Cmp};
        if (regf > 7) return std::unexpected(Error{Status::ParseError, "bad GRP1 reg"});
        out.alu_op = kAlu[regf];
        out.cls = InstrClass::Alu;
        if (op == 0x80) {
            d.opsize = 1;
            out.opsize = 1;
            out.dst.size = 1;
            auto imm = d.read_imm(1, false);
            if (!imm) return std::unexpected(imm.error());
            out.src.kind = OperandKind::Imm;
            out.src.imm = *imm;
            out.src.size = 1;
        } else {
            out.opsize = d.eff_opsize();
            out.dst.size = out.opsize;
            const size_t bytes = op == 0x81 ? (out.opsize == 2 ? 2 : 4) : 1;
            auto imm = d.read_imm(bytes, op == 0x83 || out.opsize != 2);
            if (!imm) return std::unexpected(imm.error());
            out.src.kind = OperandKind::Imm;
            out.src.imm = *imm;
            out.src.size = out.opsize;
        }
        break;
    }
    case 0x84: case 0x85: { // TEST r/m, r
        uint8_t regf;
        auto rd = d.decode_modrm(out.dst, regf);
        if (!rd) return std::unexpected(rd.error());
        out.cls = InstrClass::Test;
        out.opsize = op == 0x84 ? 1 : d.eff_opsize();
        out.dst.size = out.opsize;
        out.src.kind = OperandKind::Reg;
        out.src.reg = regf;
        out.src.high_byte = !d.pre.rex_present && op == 0x84 && regf >= 4 && regf < 8;
        out.src.size = out.opsize;
        break;
    }
    case 0x86: case 0x87: { // XCHG r/m, r
        uint8_t regf;
        auto rd = d.decode_modrm(out.dst, regf);
        if (!rd) return std::unexpected(rd.error());
        out.cls = InstrClass::Xchg;
        out.opsize = op == 0x86 ? 1 : d.eff_opsize();
        out.dst.size = out.opsize;
        out.src.kind = OperandKind::Reg;
        out.src.reg = regf;
        out.src.high_byte = !d.pre.rex_present && op == 0x86 && regf >= 4 && regf < 8;
        out.src.size = out.opsize;
        break;
    }
    case 0x88: case 0x89: case 0x8A: case 0x8B: { // MOV
        uint8_t regf;
        const bool to_mem = (op & 2) == 0;
        Operand& rmop = to_mem ? out.dst : out.src;
        auto rd = d.decode_modrm(rmop, regf);
        if (!rd) return std::unexpected(rd.error());
        out.cls = InstrClass::Mov;
        out.opsize = op == 0x88 || op == 0x8A ? 1 : d.eff_opsize();
        if (to_mem) {
            out.src.kind = OperandKind::Reg;
            out.src.reg = regf;
            out.src.high_byte = op == 0x88 && !d.pre.rex_present && regf >= 4 && regf < 8;
        } else {
            out.dst.kind = OperandKind::Reg;
            out.dst.reg = regf;
            out.dst.high_byte = op == 0x8A && !d.pre.rex_present && regf >= 4 && regf < 8;
        }
        out.dst.size = out.src.size = out.opsize;
        break;
    }
    case 0x8D: { // LEA r, m
        uint8_t regf;
        auto rd = d.decode_modrm(out.src, regf);
        if (!rd) return std::unexpected(rd.error());
        if (out.src.kind != OperandKind::Mem)
            return std::unexpected(Error{Status::ParseError, "LEA requires a memory operand"});
        out.cls = InstrClass::Lea;
        out.opsize = d.eff_opsize();
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = regf;
        out.dst.size = out.opsize;
        break;
    }
    case 0x90: { // NOP
        out.cls = InstrClass::Nop;
        break;
    }
    case 0x91: case 0x92: case 0x93: case 0x94:
    case 0x95: case 0x96: case 0x97: { // XCHG eAX, r
        out.cls = InstrClass::Xchg;
        out.opsize = 8;
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = reg::AX;
        out.dst.size = 8;
        out.src.kind = OperandKind::Reg;
        out.src.reg = (op & 7) + (d.pre.rex_b ? 8 : 0);
        out.src.size = 8;
        break;
    }
    case 0x98: { // CDQE (REX.W) / CWDE
        out.cls = InstrClass::SignExtend;
        out.opsize = d.pre.rex_w ? 8 : 4;
        break;
    }
    case 0x99: { // CQO (REX.W) / CDQ
        out.cls = InstrClass::SignExtend;
        out.opsize = d.pre.rex_w ? 8 : 4;
        out.cond = 1; // marker: sign-extend into RDX/EDX
        break;
    }
    case 0x9E: // SAHF
        return std::unexpected(Error{Status::Unsupported, "SAHF not implemented"});
    case 0x9F: // LAHF
        return std::unexpected(Error{Status::Unsupported, "LAHF not implemented"});
    case 0xA8: { // TEST AL, imm8
        out.cls = InstrClass::Test;
        out.opsize = 1;
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = reg::AX;
        out.dst.size = 1;
        auto imm = d.read_imm(1, false);
        if (!imm) return std::unexpected(imm.error());
        out.src.kind = OperandKind::Imm;
        out.src.imm = *imm;
        out.src.size = 1;
        break;
    }
    case 0xA9: { // TEST eAX, imm
        out.cls = InstrClass::Test;
        out.opsize = d.eff_opsize();
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = reg::AX;
        out.dst.size = out.opsize;
        const size_t bytes = out.opsize == 2 ? 2 : 4;
        auto imm = d.read_imm(bytes, out.opsize != 2);
        if (!imm) return std::unexpected(imm.error());
        out.src.kind = OperandKind::Imm;
        out.src.imm = *imm;
        out.src.size = out.opsize;
        break;
    }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: { // MOV r8, imm8
        out.cls = InstrClass::MovImm;
        out.opsize = 1;
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = (op & 7) + (d.pre.rex_b ? 8 : 0);
        out.dst.high_byte = !d.pre.rex_present && (op & 7) >= 4;
        out.dst.size = 1;
        auto imm = d.read_imm(1, false);
        if (!imm) return std::unexpected(imm.error());
        out.src.kind = OperandKind::Imm;
        out.src.imm = *imm;
        out.src.size = 1;
        break;
    }
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: { // MOV r, imm
        out.cls = InstrClass::MovImm;
        out.opsize = d.eff_opsize();
        out.dst.kind = OperandKind::Reg;
        out.dst.reg = (op & 7) + (d.pre.rex_b ? 8 : 0);
        out.dst.size = out.opsize;
        const size_t bytes = out.opsize == 2 ? 2 : (out.opsize == 8 ? 8 : 4);
        auto imm = d.read_imm(bytes, false);
        if (!imm) return std::unexpected(imm.error());
        out.src.kind = OperandKind::Imm;
        out.src.imm = *imm;
        out.src.size = out.opsize;
        break;
    }
    case 0xC0: case 0xC1: { // GRP2 shift r/m, imm8
        uint8_t regf;
        auto rd = d.decode_modrm(out.dst, regf);
        if (!rd) return std::unexpected(rd.error());
        if (regf == 2 || regf == 3) // RCL/RCR through carry
            return std::unexpected(Error{Status::Unsupported, "RCL/RCR not implemented"});
        out.cls = InstrClass::Shift;
        out.shift_op = static_cast<ShiftOp>(regf);
        out.opsize = op == 0xC0 ? 1 : d.eff_opsize();
        out.dst.size = out.opsize;
        auto imm = d.read_imm(1, false);
        if (!imm) return std::unexpected(imm.error());
        out.src.kind = OperandKind::Imm;
        out.src.imm = *imm;
        out.src.size = 1;
        break;
    }
    case 0xC2: { // RET imm16
        out.cls = InstrClass::Ret;
        auto imm = d.read_imm(2, false);
        if (!imm) return std::unexpected(imm.error());
        out.src.imm = *imm;
        break;
    }
    case 0xC3: { // RET
        out.cls = InstrClass::Ret;
        break;
    }
    case 0xC6: case 0xC7: { // MOV r/m, imm
        uint8_t regf;
        auto rd = d.decode_modrm(out.dst, regf);
        if (!rd) return std::unexpected(rd.error());
        if (regf != 0) return std::unexpected(Error{Status::ParseError, "bad MOV imm reg field"});
        out.cls = InstrClass::MovImm;
        if (op == 0xC6) {
            d.opsize = 1;
            out.opsize = 1;
            out.dst.size = 1;
            auto imm = d.read_imm(1, false);
            if (!imm) return std::unexpected(imm.error());
            out.src.kind = OperandKind::Imm;
            out.src.imm = *imm;
            out.src.size = 1;
        } else {
            out.opsize = d.eff_opsize();
            out.dst.size = out.opsize;
            // imm32 (imm16 with 66); sign-extended for 64-bit forms.
            const size_t bytes = out.opsize == 2 ? 2 : 4;
            auto imm = d.read_imm(bytes, out.opsize == 8);
            if (!imm) return std::unexpected(imm.error());
            out.src.kind = OperandKind::Imm;
            out.src.imm = *imm;
            out.src.size = out.opsize;
        }
        break;
    }
    case 0xD0: case 0xD1: { // GRP2 shift r/m, 1
        uint8_t regf;
        auto rd = d.decode_modrm(out.dst, regf);
        if (!rd) return std::unexpected(rd.error());
        if (regf == 2 || regf == 3)
            return std::unexpected(Error{Status::Unsupported, "RCL/RCR not implemented"});
        out.cls = InstrClass::Shift;
        out.shift_op = static_cast<ShiftOp>(regf);
        out.opsize = op == 0xD0 ? 1 : d.eff_opsize();
        out.dst.size = out.opsize;
        out.src.kind = OperandKind::Imm;
        out.src.imm = 1;
        out.src.size = 1;
        break;
    }
    case 0xD2: case 0xD3: { // GRP2 shift r/m, CL
        uint8_t regf;
        auto rd = d.decode_modrm(out.dst, regf);
        if (!rd) return std::unexpected(rd.error());
        if (regf == 2 || regf == 3)
            return std::unexpected(Error{Status::Unsupported, "RCL/RCR not implemented"});
        out.cls = InstrClass::Shift;
        out.shift_op = static_cast<ShiftOp>(regf);
        out.opsize = op == 0xD2 ? 1 : d.eff_opsize();
        out.dst.size = out.opsize;
        out.src.kind = OperandKind::Reg;
        out.src.reg = reg::CX;
        out.src.size = 1;
        break;
    }
    case 0xE8: { // CALL rel32
        out.cls = InstrClass::CallRel;
        auto rel = d.read_imm(4, true);
        if (!rel) return std::unexpected(rel.error());
        out.rel = *rel;
        break;
    }
    case 0xE9: { // JMP rel32
        out.cls = InstrClass::JmpRel;
        auto rel = d.read_imm(4, true);
        if (!rel) return std::unexpected(rel.error());
        out.rel = *rel;
        break;
    }
    case 0xEB: { // JMP rel8
        out.cls = InstrClass::JmpRel;
        auto rel = d.read_imm(1, true);
        if (!rel) return std::unexpected(rel.error());
        out.rel = *rel;
        break;
    }
    case 0xF4: // HLT
        return std::unexpected(Error{Status::Unsupported, "HLT is a privileged instruction"});
    case 0xF6: case 0xF7: { // GRP3
        uint8_t regf;
        auto rd = d.decode_modrm(out.dst, regf);
        if (!rd) return std::unexpected(rd.error());
        if (op == 0xF6) {
            d.opsize = 1;
            out.opsize = 1;
        } else {
            out.opsize = d.eff_opsize();
        }
        out.dst.size = out.opsize;
        switch (regf) {
        case 0: case 1: { // TEST r/m, imm
            out.cls = InstrClass::Test;
            const size_t bytes = op == 0xF6 ? 1 : (out.opsize == 2 ? 2 : 4);
            auto imm = d.read_imm(bytes, false);
            if (!imm) return std::unexpected(imm.error());
            out.src.kind = OperandKind::Imm;
            out.src.imm = *imm;
            out.src.size = out.opsize;
            break;
        }
        case 2: out.cls = InstrClass::Not; break;
        case 3: out.cls = InstrClass::Neg; break;
        case 4: out.cls = InstrClass::Mul; break;
        case 5: out.cls = InstrClass::IMul; break;
        case 6: out.cls = InstrClass::Div; break;
        case 7: out.cls = InstrClass::Div; out.cond = 1; break; // cond=1: signed IDIV
        default: return std::unexpected(Error{Status::ParseError, "bad GRP3 reg"});
        }
        break;
    }
    case 0xFE: case 0xFF: { // GRP5
        uint8_t regf;
        auto rd = d.decode_modrm(out.dst, regf);
        if (!rd) return std::unexpected(rd.error());
        if (op == 0xFE && regf > 1)
            return std::unexpected(Error{Status::ParseError, "bad GRP5 reg for FE"});
        switch (regf) {
        case 0: case 1: {
            out.cls = InstrClass::IncDec;
            out.cond = regf; // 0=inc, 1=dec
            out.opsize = op == 0xFE ? 1 : d.eff_opsize();
            out.dst.size = out.opsize;
            break;
        }
        case 2:
            out.cls = InstrClass::Call;
            out.opsize = 8;
            out.dst.size = 8;
            break;
        case 4:
            out.cls = InstrClass::JmpReg;
            out.opsize = 8;
            out.dst.size = 8;
            break;
        case 6:
            out.cls = InstrClass::Push;
            out.opsize = 8;
            out.dst.size = 8;
            break;
        default:
            return std::unexpected(Error{Status::Unsupported, "GRP5 reg 3/5/7 not implemented"});
        }
        break;
    }
    case 0x0F: {
        uint8_t op2;
        if (!d.take(op2)) return std::unexpected(Error{Status::ParseError, "truncated 0F opcode"});
        if (op2 == 0x05) { // SYSCALL
            out.cls = InstrClass::Syscall;
            break;
        }
        if (op2 == 0x0B) // UD2
            return std::unexpected(Error{Status::ParseError, "UD2 executed"});
        if (op2 == 0x1F) { // multi-byte NOP
            uint8_t regf;
            auto rd = d.decode_modrm(out.dst, regf);
            if (!rd) return std::unexpected(rd.error());
            out.cls = InstrClass::Nop;
            break;
        }
        if (op2 >= 0x80 && op2 <= 0x8F) { // Jcc rel32
            out.cls = InstrClass::Jcc;
            out.cond = op2 & 15;
            auto rel = d.read_imm(4, true);
            if (!rel) return std::unexpected(rel.error());
            out.rel = *rel;
            break;
        }
        if (op2 >= 0x90 && op2 <= 0x9F) { // SETcc r/m8
            uint8_t regf;
            auto rd = d.decode_modrm(out.dst, regf);
            if (!rd) return std::unexpected(rd.error());
            out.cls = InstrClass::Setcc;
            out.cond = op2 & 15;
            out.opsize = 1;
            out.dst.size = 1;
            break;
        }
        if (op2 >= 0x40 && op2 <= 0x4F) { // CMOVcc r, r/m
            uint8_t regf;
            auto rd = d.decode_modrm(out.src, regf);
            if (!rd) return std::unexpected(rd.error());
            out.cls = InstrClass::Cmovcc;
            out.cond = op2 & 15;
            out.opsize = d.eff_opsize();
            out.dst.kind = OperandKind::Reg;
            out.dst.reg = regf;
            out.dst.size = out.opsize;
            out.src.size = out.opsize;
            break;
        }
        if (op2 == 0xAF) { // IMUL r, r/m
            uint8_t regf;
            auto rd = d.decode_modrm(out.src, regf);
            if (!rd) return std::unexpected(rd.error());
            out.cls = InstrClass::IMul;
            out.imul_form = 1;
            out.opsize = d.eff_opsize();
            out.dst.kind = OperandKind::Reg;
            out.dst.reg = regf;
            out.dst.size = out.opsize;
            out.src.size = out.opsize;
            break;
        }
        if (op2 == 0xB6 || op2 == 0xB7) { // MOVZX r, r/m8|16
            uint8_t regf;
            auto rd = d.decode_modrm(out.src, regf);
            if (!rd) return std::unexpected(rd.error());
            out.cls = InstrClass::Movzx;
            out.opsize = d.eff_opsize();
            out.src.size = op2 == 0xB6 ? 1 : 2;
            out.dst.kind = OperandKind::Reg;
            out.dst.reg = regf;
            out.dst.size = out.opsize;
            break;
        }
        if (op2 == 0xBE || op2 == 0xBF) { // MOVSX r, r/m8|16
            uint8_t regf;
            auto rd = d.decode_modrm(out.src, regf);
            if (!rd) return std::unexpected(rd.error());
            out.cls = InstrClass::Movsx;
            out.opsize = d.eff_opsize();
            out.src.size = op2 == 0xBE ? 1 : 2;
            out.dst.kind = OperandKind::Reg;
            out.dst.reg = regf;
            out.dst.size = out.opsize;
            break;
        }

        // ---- SSE subset (prefix-selected) ----
        // XMM operands: dst = reg field, src = rm (reg or memory).
        d.opsize = 16;
        out.opsize = 16;
        auto sse_modrm = [&](Operand& dst, Operand& src) -> std::expected<void, Error> {
            uint8_t regf;
            auto rd = d.decode_modrm(src, regf);
            if (!rd) return std::unexpected(rd.error());
            dst.kind = OperandKind::Reg;
            dst.reg = regf;
            dst.size = 16;
            if (src.kind == OperandKind::Reg) src.size = 16;
            return {};
        };
        using F = SseForm;
        auto reject_sse = [](const char* what) {
            char msg[80];
            snprintf(msg, sizeof(msg), "SSE %s not implemented", what);
            return std::unexpected(Error{Status::Unsupported, msg});
        };

        switch (op2) {
        case 0x10: case 0x11: { // MOVUPS/MOVSS/MOVSD/MOVUPD + stores
            const bool store = op2 == 0x11;
            F f = d.pre.rep ? F::SS : d.pre.repne ? F::SD : d.pre.opsize16 ? F::DQU : F::PS;
            out.cls = InstrClass::SseMov;
            out.sse_form = f;
            Operand& dst = store ? out.dst : out.dst; // dst = reg field always
            Operand& src = out.src;
            auto r = sse_modrm(out.dst, out.src);
            if (!r) return std::unexpected(r.error());
            (void)dst;
            (void)src;
            if (store) {
                // Store forms: data flows reg-field -> rm (reg or memory), so
                // the operands always swap relative to the load layout.
                std::swap(out.dst, out.src);
                if (f == F::SS) out.dst.size = out.src.size = 4;
                else if (f == F::SD) out.dst.size = out.src.size = 8;
            } else {
                if (f == F::SS) out.src.size = out.dst.size = 4;
                else if (f == F::SD) out.src.size = out.dst.size = 8;
            }
            break;
        }
        case 0x28: case 0x29: { // MOVAPS/MOVAPD (aligned)
            const bool store = op2 == 0x29;
            out.cls = InstrClass::SseMov;
            out.sse_form = F::AP; // full 128-bit copy, 16-byte aligned
            auto r = sse_modrm(out.dst, out.src);
            if (!r) return std::unexpected(r.error());
            if (store) std::swap(out.dst, out.src); // reg-field -> rm on stores
            break;
        }
        case 0x2A: { // CVTSI2SS / CVTSI2SD
            if (d.pre.opsize16) return reject_sse("CVTPI2PS (MMX)");
            out.cls = InstrClass::SseCvt;
            out.sse_op = d.pre.rep ? SseOp::IntToF32 : SseOp::IntToF64;
            out.sse_form = d.pre.rep ? F::SS : F::SD;
            uint8_t regf;
            auto rd = d.decode_modrm(out.src, regf);
            if (!rd) return std::unexpected(rd.error());
            out.dst.kind = OperandKind::Reg;
            out.dst.reg = regf;
            out.dst.size = 16;
            out.src.size = d.pre.rex_w ? 8 : 4;
            break;
        }
        case 0x2C: case 0x2D: { // CVTTSS2SI/CVTSS2SI (and SD forms)
            if (!d.pre.rep && !d.pre.repne) return reject_sse("CVTPI2PI (MMX)");
            out.cls = InstrClass::SseCvt;
            const bool trunc = op2 == 0x2C;
            const bool f64 = d.pre.repne;
            out.sse_op = trunc ? (f64 ? SseOp::TruncF64ToI : SseOp::TruncF32ToI)
                               : (f64 ? SseOp::RoundF64ToI : SseOp::RoundF32ToI);
            out.sse_form = f64 ? F::SD : F::SS;
            uint8_t regf;
            auto rd = d.decode_modrm(out.src, regf);
            if (!rd) return std::unexpected(rd.error());
            out.dst.kind = OperandKind::Reg;
            out.dst.reg = regf;
            out.dst.size = d.pre.rex_w ? 8 : 4;
            out.src.size = 16;
            break;
        }
        case 0x2E: case 0x2F: { // UCOMISS/COMISS (+66 = SD forms)
            out.cls = InstrClass::SseComis;
            out.cond = op2 == 0x2F ? 1 : 0; // 1 = COMIS (signals QNaN), 0 = UCOMIS
            out.sse_form = d.pre.opsize16 ? F::SD : F::SS;
            auto r = sse_modrm(out.dst, out.src);
            if (!r) return std::unexpected(r.error());
            if (out.src.kind == OperandKind::Reg) out.src.size = out.sse_form == F::SD ? 8 : 4;
            break;
        }
        case 0x51: { // SQRTSS/SQRTSD/SQRTPS/SQRTPD
            out.cls = InstrClass::SseArith;
            out.sse_op = SseOp::Sqrt;
            out.sse_form = d.pre.rep ? F::SS : d.pre.repne ? F::SD : d.pre.opsize16 ? F::PD : F::PS;
            auto r = sse_modrm(out.dst, out.src);
            if (!r) return std::unexpected(r.error());
            break;
        }
        case 0x54: case 0x55: case 0x56: case 0x57: { // AND/ANDN/OR/XOR PS/PD
            out.cls = InstrClass::SseArith;
            static const SseOp kOps[4] = {SseOp::And, SseOp::Andn, SseOp::Or, SseOp::Xor};
            out.sse_op = kOps[op2 - 0x54];
            out.sse_form = d.pre.opsize16 ? F::PD : F::PS;
            auto r = sse_modrm(out.dst, out.src);
            if (!r) return std::unexpected(r.error());
            break;
        }
        case 0x58: case 0x59: case 0x5C: case 0x5E: { // ADD/MUL/SUB/DIV
            out.cls = InstrClass::SseArith;
            static const SseOp kOps[4] = {SseOp::Add, SseOp::Mul, SseOp::Sub, SseOp::Div};
            SseOp base = op2 == 0x58 ? SseOp::Add : op2 == 0x59 ? SseOp::Mul
                         : op2 == 0x5C ? SseOp::Sub : SseOp::Div;
            out.sse_op = base;
            out.sse_form = d.pre.rep ? F::SS : d.pre.repne ? F::SD : d.pre.opsize16 ? F::PD : F::PS;
            auto r = sse_modrm(out.dst, out.src);
            if (!r) return std::unexpected(r.error());
            break;
        }
        case 0x5B: { // CVTDQ2PS / CVTPS2DQ / CVTTPS2DQ
            if (d.pre.repne) return reject_sse("CVTTPS2PI (MMX)");
            out.cls = InstrClass::SseCvt;
            out.sse_op = d.pre.opsize16 ? SseOp::F32ToI32x4
                         : d.pre.rep    ? SseOp::TruncF32ToI32x4
                                        : SseOp::I32ToF32x4;
            out.sse_form = F::PS;
            auto r = sse_modrm(out.dst, out.src);
            if (!r) return std::unexpected(r.error());
            break;
        }
        case 0x6E: { // MOVD/MOVQ GPR->XMM (66 only)
            if (!d.pre.opsize16) return reject_sse("MOVD (MMX)");
            out.cls = InstrClass::SseMov;
            out.sse_form = d.pre.rex_w ? F::Q : F::D;
            uint8_t regf;
            auto rd = d.decode_modrm(out.src, regf);
            if (!rd) return std::unexpected(rd.error());
            out.dst.kind = OperandKind::Reg;
            out.dst.reg = regf;
            out.dst.size = 16;
            out.src.size = d.pre.rex_w ? 8 : 4;
            break;
        }
        case 0x6F: { // MOVDQA (66) / MOVDQU (F3)
            if (!d.pre.opsize16 && !d.pre.rep) return reject_sse("MOVQ (MMX)");
            out.cls = InstrClass::SseMov;
            out.sse_form = d.pre.opsize16 ? F::DQA : F::DQU;
            auto r = sse_modrm(out.dst, out.src);
            if (!r) return std::unexpected(r.error());
            break;
        }
        case 0x70: { // PSHUFD (66; F3/F2 variants rejected for now)
            if (!d.pre.opsize16) return reject_sse("PSHUFW/L/H");
            out.cls = InstrClass::SseShuffle;
            out.sse_form = F::DQA;
            auto r = sse_modrm(out.dst, out.src);
            if (!r) return std::unexpected(r.error());
            auto imm = d.read_imm(1, false);
            if (!imm) return std::unexpected(imm.error());
            out.imm3 = *imm;
            break;
        }
        case 0x7E: { // MOVD/MOVQ XMM->GPR/mem (66), MOVQ xmm,xmm/m64 (F3)
            if (!d.pre.opsize16 && !d.pre.rep) return reject_sse("MOVQ (MMX)");
            out.cls = InstrClass::SseMov;
            uint8_t regf;
            auto rd = d.decode_modrm(out.dst, regf);
            if (!rd) return std::unexpected(rd.error());
            out.src.kind = OperandKind::Reg;
            out.src.reg = regf;
            out.src.size = 16;
            if (d.pre.rep) { // F3 0F 7E: MOVQ xmm, xmm/m64 (zero-extends)
                out.sse_form = F::QF3;
                out.dst.size = 16;
            } else {
                out.sse_form = d.pre.rex_w ? F::Q : F::D;
                out.dst.size = d.pre.rex_w ? 8 : 4;
            }
            break;
        }
        case 0x7F: { // MOVDQA/MOVDQU store
            if (!d.pre.opsize16 && !d.pre.rep) return reject_sse("MOVQ (MMX)");
            out.cls = InstrClass::SseMov;
            out.sse_form = d.pre.opsize16 ? F::DQA : F::DQU;
            auto r = sse_modrm(out.dst, out.src);
            if (!r) return std::unexpected(r.error());
            std::swap(out.dst, out.src); // reg-field -> rm on stores
            break;
        }
        case 0xC2: { // CMPSS/CMPPS/CMPSD/CMPPD + imm8 predicate
            out.cls = InstrClass::SseCmp;
            out.sse_form = d.pre.rep ? F::SS : d.pre.repne ? F::SD : d.pre.opsize16 ? F::PD : F::PS;
            auto r = sse_modrm(out.dst, out.src);
            if (!r) return std::unexpected(r.error());
            auto imm = d.read_imm(1, false);
            if (!imm) return std::unexpected(imm.error());
            if (*imm < 0 || *imm > 7)
                return std::unexpected(Error{Status::Unsupported, "AVX CMP predicate (>7)"});
            out.sse_pred = static_cast<uint8_t>(*imm);
            break;
        }
        case 0xD4: case 0xD5: case 0xD6: case 0xF4:
        case 0xF8: case 0xF9: case 0xFA: case 0xFB:
        case 0xFC: case 0xFD: case 0xFE: { // SSE2 packed integer ops (66 only)
            if (!d.pre.opsize16) return reject_sse("packed integer (MMX)");
            out.cls = InstrClass::SseInt;
            switch (op2) {
            case 0xD4: out.sse_op = SseOp::PaddQ; break;
            case 0xD5: out.sse_op = SseOp::Pmullw; break;
            case 0xD6: { // MOVQ xmm -> xmm/m64 (store low 64)
                out.cls = InstrClass::SseMov;
                out.sse_form = F::QD6;
                auto r = sse_modrm(out.dst, out.src);
                if (!r) return std::unexpected(r.error());
                std::swap(out.dst, out.src); // reg-field -> rm on stores
                out.dst.size = out.src.size = 8;
                break;
            }
            case 0xF4: out.sse_op = SseOp::Pmuludq; break;
            case 0xF8: out.sse_op = SseOp::PsubB; break;
            case 0xF9: out.sse_op = SseOp::PsubW; break;
            case 0xFA: out.sse_op = SseOp::PsubD; break;
            case 0xFB: out.sse_op = SseOp::PsubQ; break;
            case 0xFC: out.sse_op = SseOp::PaddB; break;
            case 0xFD: out.sse_op = SseOp::PaddW; break;
            case 0xFE: out.sse_op = SseOp::PaddD; break;
            }
            if (out.cls == InstrClass::SseInt) {
                auto r = sse_modrm(out.dst, out.src);
                if (!r) return std::unexpected(r.error());
            }
            break;
        }
        default: {
            char msg[64];
            snprintf(msg, sizeof(msg), "0F %02X not implemented", op2);
            return std::unexpected(Error{Status::Unsupported, msg});
        }
        }
        break;
    }
    default: {
        char msg[64];
        snprintf(msg, sizeof(msg), "opcode %02X not implemented", op);
        return std::unexpected(Error{Status::Unsupported, msg});
    }
    }

    out.length = static_cast<uint8_t>(d.pos);
    if (out.length == 0)
        return std::unexpected(Error{Status::ParseError, "zero-length instruction"});
    return out;
}

const char* class_name(InstrClass cls) {
    switch (cls) {
    case InstrClass::Invalid: return "invalid";
    case InstrClass::Alu: return "alu";
    case InstrClass::Mov: return "mov";
    case InstrClass::MovImm: return "mov-imm";
    case InstrClass::Lea: return "lea";
    case InstrClass::Push: return "push";
    case InstrClass::Pop: return "pop";
    case InstrClass::Call: return "call";
    case InstrClass::CallRel: return "call-rel";
    case InstrClass::Ret: return "ret";
    case InstrClass::Jcc: return "jcc";
    case InstrClass::JmpRel: return "jmp-rel";
    case InstrClass::JmpReg: return "jmp-reg";
    case InstrClass::Setcc: return "setcc";
    case InstrClass::Cmovcc: return "cmovcc";
    case InstrClass::Test: return "test";
    case InstrClass::Shift: return "shift";
    case InstrClass::Mul: return "mul";
    case InstrClass::IMul: return "imul";
    case InstrClass::Div: return "div";
    case InstrClass::Neg: return "neg";
    case InstrClass::Not: return "not";
    case InstrClass::IncDec: return "inc/dec";
    case InstrClass::Movzx: return "movzx";
    case InstrClass::Movsx: return "movsx";
    case InstrClass::Movsxd: return "movsxd";
    case InstrClass::Xchg: return "xchg";
    case InstrClass::SignExtend: return "sign-extend";
    case InstrClass::Syscall: return "syscall";
    case InstrClass::Nop: return "nop";
    case InstrClass::SseMov: return "sse-mov";
    case InstrClass::SseArith: return "sse-arith";
    case InstrClass::SseCmp: return "sse-cmp";
    case InstrClass::SseComis: return "sse-comis";
    case InstrClass::SseCvt: return "sse-cvt";
    case InstrClass::SseInt: return "sse-int";
    case InstrClass::SseShuffle: return "sse-shuffle";
    }
    return "?";
}

const char* alu_name(AluOp op) {
    switch (op) {
    case AluOp::Add: return "add";
    case AluOp::Or: return "or";
    case AluOp::Adc: return "adc";
    case AluOp::Sbb: return "sbb";
    case AluOp::And: return "and";
    case AluOp::Sub: return "sub";
    case AluOp::Xor: return "xor";
    case AluOp::Cmp: return "cmp";
    }
    return "?";
}

const char* cond_name(uint8_t cond) {
    static const char* kNames[16] = {"o", "no", "b", "ae", "e", "ne", "be", "a",
                                     "s", "ns", "p", "np", "l", "ge", "le", "g"};
    return kNames[cond & 15];
}

const char* sse_pred_name(uint8_t pred) {
    static const char* kNames[8] = {"eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"};
    return kNames[pred & 7];
}

} // namespace armsx5::cpu::x86
