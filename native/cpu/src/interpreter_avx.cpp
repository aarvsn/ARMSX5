// AVX (VEX) execution for the ARMSX5 interpreter (methods declared in
// interpreter.h).
//
// Semantics verified against real AVX hardware (scripts/verify_avx.c run on
// an AVX2 host) and the Intel SDM:
//   * VEX.128 instructions zero YMM[255:128] of the destination register —
//     including VMOVSS/VMOVSD (register form) and VCVTSI2SS, unlike legacy
//     SSE which leaves the upper half untouched.
//   * Scalar binary ops (VADDSS et al): DEST[31:0] = SRC1 op SRC2;
//     DEST[127:32] = SRC1[127:32].
//   * Scalar unary ops (VSQRTSS/VSQRTSD): DEST[31:0] = sqrt(SRC2);
//     DEST[127:32] = SRC1[127:32]. Packed sqrt takes every lane from SRC2
//     and does not read vvvv (hardware #UDs when it is not 1111b; this
//     interpreter is lenient and ignores it).
//   * VMOVSS/VMOVSD memory-source form zeroes DEST[127:32] (legacy MOVSS
//     preserves it) and the upper YMM half.
//   * VMOVD/VMOVQ zero every bit above the moved lane, including the upper
//     YMM half.
//   * VPSHUFD shuffles the two 128-bit halves independently.
// Aligned moves enforce natural alignment (16 bytes at VEX.128, 32 bytes at
// VEX.256): hardware #GP becomes a guest fault here.
#include "armsx5/cpu/interpreter.h"
#include "vec_ops.h"

#include <array>
#include <cmath>
#include <cstring>

namespace armsx5::cpu {
namespace {

using x86::OperandKind;
using x86::SseForm;
using x86::SseOp;
using namespace vecops;

// Whether the form operates on packed (vector) data rather than a scalar.
bool packed_form(SseForm f) {
    return f == SseForm::PS || f == SseForm::PD || f == SseForm::DQA ||
           f == SseForm::DQU || f == SseForm::AP;
}

// Memory footprint of a form at the given vector length.
size_t mem_bytes(SseForm f, bool wide) {
    switch (f) {
    case SseForm::SS: return 4;
    case SseForm::SD: return 8;
    case SseForm::D: return 4;
    case SseForm::Q:
    case SseForm::QF3:
    case SseForm::QD6: return 8;
    default: return wide ? 32 : 16;
    }
}

// Natural alignment (bytes) required by a form, 0 = unaligned.
size_t form_alignment(SseForm f, bool wide) {
    if (f == SseForm::DQA || f == SseForm::AP) return wide ? 32 : 16;
    return 0;
}

} // namespace

// Loads the AVX source operand. Register sources return the full register
// width (wide for packed VEX.256 forms); memory sources read only the
// form's footprint and zero the rest.
std::expected<Vec256, Error> Interpreter::avx_load_source(const x86::Decoded& d) {
    const bool wide = d.vex_l && packed_form(d.sse_form);
    if (d.src.kind == OperandKind::Reg) return read_vec(d.src.reg & 15, wide);
    if (d.src.kind != OperandKind::Mem)
        return std::unexpected(Error{Status::InternalError, "bad AVX source operand"});

    const uint64_t addr = compute_addr(d.src.mem, d.length);
    const size_t align = form_alignment(d.sse_form, d.vex_l);
    if (align != 0 && (addr % align) != 0)
        return std::unexpected(
            Error{Status::GuestFault, "AVX aligned access requires natural alignment"});

    const size_t bytes = mem_bytes(d.sse_form, d.vex_l);
    Vec256 v{};
    if (bytes == 32) {
        auto r = read_mem256(addr);
        if (!r) return std::unexpected(r.error());
        return *r;
    }
    if (bytes == 16) {
        auto r = read_mem128(addr);
        if (!r) return std::unexpected(r.error());
        v.lo = *r;
        return v;
    }
    uint64_t scalar = 0;
    auto r = mem_.read(addr, &scalar, bytes);
    if (!r) return std::unexpected(r.error());
    v.lo.lo = scalar;
    return v;
}

std::expected<void, Error> Interpreter::execute_avx_mov(x86::Decoded& d) {
    using SF = SseForm;
    switch (d.sse_form) {
    case SF::SS:
    case SF::SD: {
        const size_t bytes = d.sse_form == SF::SS ? 4 : 8;
        if (d.src.kind == OperandKind::Mem) {
            // VMOVSS/VMOVSD xmm1, m32/m64: lane0 = m, all other bits of the
            // destination zeroed (hardware rule; legacy MOVSS preserves the
            // upper lanes instead).
            auto srcv = avx_load_source(d);
            if (!srcv) return std::unexpected(srcv.error());
            write_vec(d.dst.reg & 15, false, *srcv);
            return {};
        }
        if (d.dst.kind == OperandKind::Mem) {
            // Store form: memory gets the low scalar only.
            const uint64_t addr = compute_addr(d.dst.mem, d.length);
            const Vec256 srcv = read_vec(d.src.reg & 15, false);
            const uint64_t scalar =
                bytes == 4 ? (srcv.lo.lo & 0xFFFFFFFFULL) : srcv.lo.lo;
            return mem_.write(addr, &scalar, bytes);
        }
        // Register 3-operand form: DEST[31:0] = SRC2[31:0];
        // DEST[127:32] = SRC1[127:32]; upper YMM half zeroed.
        const Vec256 s1 = read_vec(d.vvvv & 15, false);
        const Vec256 s2 = read_vec(d.src.reg & 15, false);
        Vec256 r = s1;
        const uint64_t mask = bytes == 4 ? 0xFFFFFFFFULL : ~0ULL;
        r.lo.lo = (r.lo.lo & ~mask) | (s2.lo.lo & mask);
        write_vec(d.dst.reg & 15, false, r);
        return {};
    }
    case SF::D: {
        // VMOVD xmm1, r/m32: zero-extends above lane 0 (and YMM upper).
        // The source is a GPR or memory operand (NOT a vector register).
        if (d.dst.size == 16) {
            uint32_t scalar = 0;
            if (d.src.kind == OperandKind::Mem) {
                const uint64_t addr = compute_addr(d.src.mem, d.length);
                auto r = mem_.read(addr, &scalar, 4);
                if (!r) return std::unexpected(r.error());
            } else {
                scalar = static_cast<uint32_t>(state_.gpr[d.src.reg & 15]);
            }
            Vec256 v{};
            v.lo.lo = scalar;
            write_vec(d.dst.reg & 15, false, v);
            return {};
        }
        // VMOVD r/m32, xmm1: store the low dword (GPR write zero-extends).
        const uint32_t low = static_cast<uint32_t>(state_.xmm[d.src.reg & 15].lo);
        if (d.dst.kind == OperandKind::Mem) {
            const uint64_t addr = compute_addr(d.dst.mem, d.length);
            return mem_.write(addr, &low, 4);
        }
        write_reg(state_, d.dst.reg & 15, 4, low);
        return {};
    }
    case SF::Q: {
        // VMOVQ xmm1, r/m64 (66 W1): zero-extends above lane 0. The source
        // is a GPR or memory operand (NOT a vector register).
        if (d.dst.size == 16) {
            uint64_t scalar = 0;
            if (d.src.kind == OperandKind::Mem) {
                const uint64_t addr = compute_addr(d.src.mem, d.length);
                auto r = mem_.read(addr, &scalar, 8);
                if (!r) return std::unexpected(r.error());
            } else {
                scalar = state_.gpr[d.src.reg & 15];
            }
            Vec256 v{};
            v.lo.lo = scalar;
            write_vec(d.dst.reg & 15, false, v);
            return {};
        }
        // VMOVQ r/m64, xmm1: store the low qword.
        const uint64_t low = state_.xmm[d.src.reg & 15].lo;
        if (d.dst.kind == OperandKind::Mem) {
            const uint64_t addr = compute_addr(d.dst.mem, d.length);
            return mem_.write(addr, &low, 8);
        }
        write_reg(state_, d.dst.reg & 15, 8, low);
        return {};
    }
    case SF::QF3: {
        // VMOVQ xmm1, xmm2/m64 (F3 7E): zero-extend to 128 bits.
        if (d.dst.kind == OperandKind::Mem) {
            const uint64_t addr = compute_addr(d.dst.mem, d.length);
            const uint64_t low = state_.xmm[d.src.reg & 15].lo;
            return mem_.write(addr, &low, 8);
        }
        auto srcv = avx_load_source(d);
        if (!srcv) return std::unexpected(srcv.error());
        Vec256 v{};
        v.lo.lo = srcv->lo.lo;
        write_vec(d.dst.reg & 15, false, v);
        return {};
    }
    case SF::QD6: {
        // VMOVQ xmm/m64, xmm1 (66 D6): store the low qword.
        if (d.dst.kind == OperandKind::Mem) {
            const uint64_t addr = compute_addr(d.dst.mem, d.length);
            const uint64_t low = state_.xmm[d.src.reg & 15].lo;
            return mem_.write(addr, &low, 8);
        }
        state_.xmm[d.dst.reg & 15].lo = state_.xmm[d.src.reg & 15].lo;
        return {};
    }
    case SF::PS:
    case SF::PD:
    case SF::DQA:
    case SF::DQU:
    case SF::AP: {
        const bool wide = d.vex_l;
        if (d.dst.kind == OperandKind::Mem) {
            const uint64_t addr = compute_addr(d.dst.mem, d.length);
            const size_t align = form_alignment(d.sse_form, wide);
            if (align != 0 && (addr % align) != 0)
                return std::unexpected(
                    Error{Status::GuestFault, "AVX aligned access requires natural alignment"});
            const Vec256 srcv = read_vec(d.src.reg & 15, wide);
            return wide ? write_mem256(addr, srcv) : write_mem128(addr, srcv.lo);
        }
        Vec256 v{};
        if (d.src.kind == OperandKind::Mem) {
            auto r = avx_load_source(d);
            if (!r) return std::unexpected(r.error());
            v = *r;
        } else {
            v = read_vec(d.src.reg & 15, wide);
        }
        write_vec(d.dst.reg & 15, wide, v);
        return {};
    }
    }
    return std::unexpected(Error{Status::InternalError, "unhandled AVX move form"});
}

std::expected<void, Error> Interpreter::execute_avx_arith(x86::Decoded& d) {
    using SF = SseForm;
    auto src2 = avx_load_source(d);
    if (!src2) return std::unexpected(src2.error());
    const bool wide = d.vex_l && packed_form(d.sse_form);
    const Vec256 s1 = read_vec(d.vvvv & 15, wide); // packed forms; scalar uses low 128
    const Vec256 s2 = *src2;
    const bool scalar = !packed_form(d.sse_form);
    const bool f32_form = d.sse_form == SF::SS || d.sse_form == SF::PS;
    const int lanes = scalar ? 1
                             : f32_form ? (wide ? 8 : 4)
                                        : (wide ? 4 : 2);
    Vec256 r{};

    if (d.sse_op == SseOp::And || d.sse_op == SseOp::Andn || d.sse_op == SseOp::Or ||
        d.sse_op == SseOp::Xor) {
        switch (d.sse_op) {
        case SseOp::And:
            r.lo = XmmValue{s1.lo.lo & s2.lo.lo, s1.lo.hi & s2.lo.hi};
            r.hi = XmmValue{s1.hi.lo & s2.hi.lo, s1.hi.hi & s2.hi.hi};
            break;
        case SseOp::Andn: // DEST = NOT(SRC1) AND SRC2
            r.lo = XmmValue{~s1.lo.lo & s2.lo.lo, ~s1.lo.hi & s2.lo.hi};
            r.hi = XmmValue{~s1.hi.lo & s2.hi.lo, ~s1.hi.hi & s2.hi.hi};
            break;
        case SseOp::Or:
            r.lo = XmmValue{s1.lo.lo | s2.lo.lo, s1.lo.hi | s2.lo.hi};
            r.hi = XmmValue{s1.hi.lo | s2.hi.lo, s1.hi.hi | s2.hi.hi};
            break;
        default:
            r.lo = XmmValue{s1.lo.lo ^ s2.lo.lo, s1.lo.hi ^ s2.lo.hi};
            r.hi = XmmValue{s1.hi.lo ^ s2.hi.lo, s1.hi.hi ^ s2.hi.hi};
            break;
        }
        write_vec(d.dst.reg & 15, wide, r);
        return {};
    }

    if (d.sse_op == SseOp::Sqrt) {
        // Scalar: DEST[31:0] = sqrt(SRC2[31:0]), DEST[127:32] = SRC1.
        // Packed: every lane from SRC2 (SRC1/vvvv unused).
        if (scalar) r = s1;
        for (int i = 0; i < lanes; ++i) {
            if (f32_form)
                set_vec_lane_f32(r, i, std::sqrt(vec_lane_f32(s2, i)));
            else
                set_vec_lane_f64(r, i, std::sqrt(vec_lane_f64(s2, i)));
        }
        write_vec(d.dst.reg & 15, wide, r);
        return {};
    }

    // Binary packed/scalar float ops.
    if (scalar) r = s1; // scalar: upper lanes of DEST copied from SRC1
    switch (d.sse_op) {
    case SseOp::Add:
        for (int i = 0; i < lanes; ++i) {
            if (f32_form)
                set_vec_lane_f32(r, i, vec_lane_f32(s1, i) + vec_lane_f32(s2, i));
            else
                set_vec_lane_f64(r, i, vec_lane_f64(s1, i) + vec_lane_f64(s2, i));
        }
        break;
    case SseOp::Sub:
        for (int i = 0; i < lanes; ++i) {
            if (f32_form)
                set_vec_lane_f32(r, i, vec_lane_f32(s1, i) - vec_lane_f32(s2, i));
            else
                set_vec_lane_f64(r, i, vec_lane_f64(s1, i) - vec_lane_f64(s2, i));
        }
        break;
    case SseOp::Mul:
        for (int i = 0; i < lanes; ++i) {
            if (f32_form)
                set_vec_lane_f32(r, i, vec_lane_f32(s1, i) * vec_lane_f32(s2, i));
            else
                set_vec_lane_f64(r, i, vec_lane_f64(s1, i) * vec_lane_f64(s2, i));
        }
        break;
    case SseOp::Div:
        for (int i = 0; i < lanes; ++i) {
            if (f32_form)
                set_vec_lane_f32(r, i, vec_lane_f32(s1, i) / vec_lane_f32(s2, i));
            else
                set_vec_lane_f64(r, i, vec_lane_f64(s1, i) / vec_lane_f64(s2, i));
        }
        break;
    default:
        return std::unexpected(Error{Status::InternalError, "bad AVX arith op"});
    }
    write_vec(d.dst.reg & 15, wide, r);
    return {};
}

std::expected<void, Error> Interpreter::execute_avx_cmp(x86::Decoded& d) {
    using SF = SseForm;
    auto src2 = avx_load_source(d);
    if (!src2) return std::unexpected(src2.error());
    const bool wide = d.vex_l && packed_form(d.sse_form);
    const Vec256 s1 = read_vec(d.vvvv & 15, wide);
    const Vec256 s2 = *src2;
    const bool scalar = !packed_form(d.sse_form);
    const bool f32_form = d.sse_form == SF::SS || d.sse_form == SF::PS;
    const int lanes = scalar ? 1 : f32_form ? (wide ? 8 : 4) : (wide ? 4 : 2);
    const uint8_t pred = d.sse_pred & 7;

    auto cmp_bool = [](auto a, auto b, uint8_t p) {
        const bool nan = is_nan(a) || is_nan(b);
        switch (p) {
        case 0: return a == b;           // EQ
        case 1: return !nan && a < b;    // LT
        case 2: return !nan && a <= b;   // LE
        case 3: return nan;              // UNORD
        case 4: return !(a == b);        // NEQ (true if unordered too)
        case 5: return !(nan || a < b);  // NLT
        case 6: return !(nan || a <= b); // NLE
        case 7: return !nan;             // ORD
        }
        return false;
    };

    Vec256 r = scalar ? s1 : Vec256{};
    for (int i = 0; i < lanes; ++i) {
        const bool ok = f32_form ? cmp_bool(vec_lane_f32(s1, i), vec_lane_f32(s2, i), pred)
                                 : cmp_bool(vec_lane_f64(s1, i), vec_lane_f64(s2, i), pred);
        if (f32_form)
            set_vec_lane_u32(r, i, ok ? 0xFFFFFFFFu : 0u);
        else
            set_vec_lane_u64(r, i, ok ? ~0ULL : 0ULL);
    }
    write_vec(d.dst.reg & 15, wide, r);
    return {};
}

std::expected<void, Error> Interpreter::execute_avx_comis(x86::Decoded& d) {
    auto srcv = avx_load_source(d);
    if (!srcv) return std::unexpected(srcv.error());
    const Vec256 a = read_vec(d.dst.reg & 15, false);
    Flags& f = state_.flags;
    f.sf = f.of = f.af = false;
    if (d.sse_form == SseForm::SS) {
        const float x = vec_lane_f32(a, 0), y = vec_lane_f32(*srcv, 0);
        if (is_nan(x) || is_nan(y)) f.zf = f.pf = f.cf = true;
        else {
            f.zf = x == y;
            f.pf = false;
            f.cf = x < y;
        }
    } else {
        const double x = vec_lane_f64(a, 0), y = vec_lane_f64(*srcv, 0);
        if (is_nan(x) || is_nan(y)) f.zf = f.pf = f.cf = true;
        else {
            f.zf = x == y;
            f.pf = false;
            f.cf = x < y;
        }
    }
    return {};
}

std::expected<void, Error> Interpreter::execute_avx_cvt(x86::Decoded& d) {
    using SF = SseForm;

    switch (d.sse_op) {
    case SseOp::IntToF32:
    case SseOp::IntToF64: {
        // VCVTSI2SS/VCVTSI2SD xmm1, xmm2, r/m32|m64: SRC2 is a GPR or memory
        // operand (not a vector); DEST lane0 = convert(SRC2);
        // DEST[127:32] = SRC1 (vvvv); upper YMM half zeroed.
        uint64_t raw = 0;
        if (d.src.kind == OperandKind::Mem) {
            const uint64_t addr = compute_addr(d.src.mem, d.length);
            auto r = mem_.read(addr, &raw, d.src.size);
            if (!r) return std::unexpected(r.error());
        } else {
            raw = state_.gpr[d.src.reg & 15];
        }
        const int64_t iv = d.src.size == 8 ? static_cast<int64_t>(raw)
                                           : static_cast<int32_t>(raw);
        const Vec256 s1 = read_vec(d.vvvv & 15, false);
        Vec256 r = s1;
        if (d.sse_op == SseOp::IntToF32)
            set_vec_lane_f32(r, 0, static_cast<float>(iv));
        else
            set_vec_lane_f64(r, 0, static_cast<double>(iv));
        write_vec(d.dst.reg & 15, false, r);
        return {};
    }
    case SseOp::TruncF32ToI:
    case SseOp::TruncF64ToI:
    case SseOp::RoundF32ToI:
    case SseOp::RoundF64ToI: {
        // VCVTTSS2SI/VCVTTSD2SI/VCVTSS2SI/VCVTSD2SI: GPR destination; vvvv
        // unused. Identical semantics to the legacy forms.
        auto srcv = avx_load_source(d);
        if (!srcv) return std::unexpected(srcv.error());
        const bool trunc = d.sse_op == SseOp::TruncF32ToI || d.sse_op == SseOp::TruncF64ToI;
        const bool f64_in = d.sse_form == SF::SD;
        if (f64_in) {
            const double v = vec_lane_f64(*srcv, 0);
            const double rr = trunc ? v : std::nearbyint(v);
            if (is_nan(v) || rr >= 9223372036854775808.0 || rr < -9223372036854775808.0) {
                write_reg(state_, d.dst.reg & 15, d.dst.size, kIntIndefinite64);
                return {};
            }
            write_reg(state_, d.dst.reg & 15, d.dst.size,
                      static_cast<uint64_t>(static_cast<int64_t>(rr)));
            return {};
        }
        const float v = vec_lane_f32(*srcv, 0);
        const double rr = trunc ? static_cast<double>(v) : std::nearbyint(static_cast<double>(v));
        if (is_nan(v) || rr >= 2147483648.0 || rr < -2147483648.0) {
            if (d.dst.size == 8) {
                // A 64-bit GPR destination takes the 32-bit indefinite
                // value zero-extended (32-bit source conversion).
                write_reg(state_, d.dst.reg & 15, 8, kIntIndefinite32);
            } else {
                write_reg(state_, d.dst.reg & 15, 4, kIntIndefinite32);
            }
            return {};
        }
        const int64_t r = static_cast<int64_t>(rr);
        if (d.dst.size == 8) write_reg(state_, d.dst.reg & 15, 8, static_cast<uint64_t>(r));
        else write_reg(state_, d.dst.reg & 15, 4, static_cast<uint32_t>(static_cast<int32_t>(r)));
        return {};
    }
    case SseOp::I32ToF32x4:
    case SseOp::F32ToI32x4:
    case SseOp::TruncF32ToI32x4: {
        // VCVTDQ2PS/VCVTPS2DQ/VCVTTPS2DQ: every lane from SRC2 (vvvv
        // unused); VEX.128 zeroes the upper YMM half.
        auto srcv = avx_load_source(d);
        if (!srcv) return std::unexpected(srcv.error());
        const Vec256 s = *srcv;
        const bool wide = d.vex_l;
        const int lanes = wide ? 8 : 4;
        Vec256 r{};
        for (int i = 0; i < lanes; ++i) {
            const float f = vec_lane_f32(s, i);
            if (d.sse_op == SseOp::I32ToF32x4) {
                set_vec_lane_f32(r, i, static_cast<float>(static_cast<int32_t>(vec_lane_u32(s, i))));
            } else if (is_nan(f) || f >= 2147483648.0f || f < -2147483648.0f) {
                set_vec_lane_u32(r, i, kIntIndefinite32);
            } else if (d.sse_op == SseOp::TruncF32ToI32x4) {
                set_vec_lane_u32(r, i, static_cast<uint32_t>(static_cast<int32_t>(f)));
            } else {
                set_vec_lane_u32(r, i,
                                 static_cast<uint32_t>(static_cast<int32_t>(std::nearbyint(f))));
            }
        }
        write_vec(d.dst.reg & 15, wide, r);
        return {};
    }
    default:
        return std::unexpected(Error{Status::InternalError, "bad AVX cvt op"});
    }
}

std::expected<void, Error> Interpreter::execute_avx_int(x86::Decoded& d) {
    auto srcv = avx_load_source(d);
    if (!srcv) return std::unexpected(srcv.error());
    const bool wide = d.vex_l;
    const Vec256 s1 = read_vec(d.vvvv & 15, wide); // SRC1 = vvvv register
    const Vec256 s2 = *srcv;
    Vec256 r{};

    switch (d.sse_op) {
    case SseOp::PaddB:
    case SseOp::PsubB: {
        uint8_t a[32], b[32];
        std::memcpy(a, &s1, 32);
        std::memcpy(b, &s2, 32);
        for (int i = 0; i < 32; ++i) {
            a[i] = d.sse_op == SseOp::PaddB
                       ? static_cast<uint8_t>(a[i] + b[i])
                       : static_cast<uint8_t>(a[i] - b[i]);
        }
        std::memcpy(&r, a, 32);
        break;
    }
    case SseOp::PaddW:
    case SseOp::PsubW: {
        uint16_t a[16], b[16];
        std::memcpy(a, &s1, 32);
        std::memcpy(b, &s2, 32);
        for (int i = 0; i < 16; ++i) {
            a[i] = d.sse_op == SseOp::PaddW
                       ? static_cast<uint16_t>(a[i] + b[i])
                       : static_cast<uint16_t>(a[i] - b[i]);
        }
        std::memcpy(&r, a, 32);
        break;
    }
    case SseOp::PaddD:
    case SseOp::PsubD: {
        for (int i = 0; i < 8; ++i) {
            const uint32_t x = vec_lane_u32(s1, i), y = vec_lane_u32(s2, i);
            set_vec_lane_u32(r, i, d.sse_op == SseOp::PaddD ? x + y : x - y);
        }
        break;
    }
    case SseOp::PaddQ:
    case SseOp::PsubQ: {
        for (int i = 0; i < 4; ++i) {
            const uint64_t x = vec_lane_u64(s1, i), y = vec_lane_u64(s2, i);
            set_vec_lane_u64(r, i, d.sse_op == SseOp::PaddQ ? x + y : x - y);
        }
        break;
    }
    case SseOp::Pmullw: {
        int16_t a[16], b[16];
        std::memcpy(a, &s1, 32);
        std::memcpy(b, &s2, 32);
        uint16_t out[16];
        for (int i = 0; i < 16; ++i) {
            const int32_t p = static_cast<int32_t>(a[i]) * b[i];
            out[i] = static_cast<uint16_t>(p & 0xFFFF);
        }
        std::memcpy(&r, out, 32);
        break;
    }
    case SseOp::Pmuludq: {
        // qword k of DEST = u32(SRC1 lane 2k) * u32(SRC2 lane 2k);
        // 2 products at 128 bits, 4 at 256 bits.
        const int pairs = wide ? 4 : 2;
        for (int k = 0; k < pairs; ++k) {
            const uint32_t a = vec_lane_u32(s1, 2 * k);
            const uint32_t b = vec_lane_u32(s2, 2 * k);
            set_vec_lane_u64(r, k, static_cast<uint64_t>(a) * b);
        }
        break;
    }
    default:
        return std::unexpected(Error{Status::InternalError, "bad AVX int op"});
    }
    write_vec(d.dst.reg & 15, wide, r);
    return {};
}

std::expected<void, Error> Interpreter::execute_avx_shuffle(x86::Decoded& d) {
    // VPSHUFD: dword lane i = source dword lane (imm >> 2i) & 3 within each
    // 128-bit half; the two halves shuffle independently.
    auto srcv = avx_load_source(d);
    if (!srcv) return std::unexpected(srcv.error());
    const Vec256 s = *srcv;
    const bool wide = d.vex_l;
    const int lanes = wide ? 8 : 4;
    const uint8_t imm = static_cast<uint8_t>(d.imm3 & 0xFF);
    Vec256 r{};
    for (int i = 0; i < lanes; ++i) {
        const int half = i / 4;
        const int idx = half * 4 + ((imm >> (2 * (i % 4))) & 3);
        set_vec_lane_u32(r, i, vec_lane_u32(s, idx));
    }
    write_vec(d.dst.reg & 15, wide, r);
    return {};
}

} // namespace armsx5::cpu
