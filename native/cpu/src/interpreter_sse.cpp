// SSE execution for the ARMSX5 interpreter (methods declared in
// interpreter.h). Float semantics: IEEE-754 with the default round-to-
// nearest-even mode; MXCSR exception flags are not modeled (documented in
// docs/architecture/cpu.md). Aligned 128-bit moves enforce 16-byte alignment
// like real hardware (hardware #GP becomes a guest fault here).
// AVX (VEX-encoded) execution lives in interpreter_avx.cpp.
#include "armsx5/cpu/interpreter.h"
#include "vec_ops.h"

#include <array>
#include <bit>

namespace armsx5::cpu {
namespace {

using x86::OperandKind;
using x86::SseForm;
using x86::SseOp;
using namespace vecops;

} // namespace

// Loads the SSE source operand into a 128-bit value (scalar forms occupy the
// low lanes; upper bits are zero for values read from memory).
std::expected<XmmValue, Error> Interpreter::sse_load_source(const x86::Decoded& d) {
    XmmValue v{};
    if (d.src.kind == OperandKind::Reg) {
        v = state_.xmm[d.src.reg & 15];
        return v;
    }
    if (d.src.kind != OperandKind::Mem)
        return std::unexpected(Error{Status::InternalError, "bad SSE source operand"});
    const uint64_t addr = compute_addr(d.src.mem, d.length);
    size_t bytes = 16;
    switch (d.sse_form) {
    case SseForm::SS: bytes = 4; break;
    case SseForm::SD: bytes = 8; break;
    case SseForm::D: bytes = 4; break;
    case SseForm::Q: bytes = 8; break;
    case SseForm::PS: case SseForm::PD: case SseForm::DQA: case SseForm::DQU: bytes = 16; break;
    }
    if (d.sse_form == SseForm::DQA && (addr % 16) != 0)
        return std::unexpected(
            Error{Status::GuestFault, "SSE aligned access requires 16-byte alignment"});
    if (bytes == 16) {
        auto r = read_mem128(addr);
        if (!r) return std::unexpected(r.error());
        return *r;
    }
    uint64_t scalar = 0;
    auto r = mem_.read(addr, &scalar, bytes);
    if (!r) return std::unexpected(r.error());
    v.lo = scalar;
    return v;
}

std::expected<void, Error> Interpreter::execute_sse_mov(x86::Decoded& d) {
    using SF = SseForm;
    // Load forms: dst = xmm reg, source = reg/mem.
    // Store forms: dst = reg/mem, source = xmm reg (decoder normalized).
    auto& dst_xmm = state_.xmm[d.dst.reg & 15];

    switch (d.sse_form) {
    case SF::SS:
    case SF::SD: {
        const size_t bytes = d.sse_form == SF::SS ? 4 : 8;
        if (d.src.kind == OperandKind::Mem) {
            // load scalar into lane 0, preserve the rest of dst
            const uint64_t addr = compute_addr(d.src.mem, d.length);
            uint64_t scalar = 0;
            auto r = mem_.read(addr, &scalar, bytes);
            if (!r) return std::unexpected(r.error());
            if (bytes == 4) {
                dst_xmm.lo = (dst_xmm.lo & 0xFFFFFFFF00000000ULL) | scalar;
            } else {
                dst_xmm.lo = scalar;
            }
        } else if (d.dst.kind == OperandKind::Mem) {
            const uint64_t addr = compute_addr(d.dst.mem, d.length);
            const XmmValue& srcv = state_.xmm[d.src.reg & 15];
            const uint64_t scalar = bytes == 4 ? (srcv.lo & 0xFFFFFFFFULL) : srcv.lo;
            return mem_.write(addr, &scalar, bytes);
        } else {
            // reg -> reg: lane 0 moves, dst upper preserved
            const XmmValue& srcv = state_.xmm[d.src.reg & 15];
            if (bytes == 4) {
                dst_xmm.lo = (dst_xmm.lo & 0xFFFFFFFF00000000ULL) | (srcv.lo & 0xFFFFFFFFULL);
            } else {
                dst_xmm.lo = srcv.lo;
            }
        }
        return {};
    }
    case SF::D: {
        // Direction comes from which side is the 128-bit register:
        //   dst.size == 16 -> 66 0F 6E: xmm <- GPR/mem32 (upper bits cleared)
        //   otherwise      -> 66 0F 7E: GPR/mem32 <- xmm low 32
        if (d.dst.size == 16) {
            uint32_t scalar = 0;
            if (d.src.kind == OperandKind::Mem) {
                const uint64_t addr = compute_addr(d.src.mem, d.length);
                auto r = mem_.read(addr, &scalar, 4);
                if (!r) return std::unexpected(r.error());
            } else {
                scalar = static_cast<uint32_t>(state_.gpr[d.src.reg & 15]);
            }
            dst_xmm.lo = scalar;
            dst_xmm.hi = 0;
            return {};
        }
        const uint32_t low = lane_u32(state_.xmm[d.src.reg & 15], 0);
        if (d.dst.kind == OperandKind::Mem) {
            const uint64_t addr = compute_addr(d.dst.mem, d.length);
            return mem_.write(addr, &low, 4);
        }
        write_reg(state_, d.dst.reg & 15, 4, low);
        return {};
    }
    case SF::Q: {
        // 66 REX.W 0F 6E: xmm <- GPR/mem64 (upper cleared)
        // 66 REX.W 0F 7E: GPR <- xmm low 64
        // 66 0F D6 (QD6): xmm/m64 <- xmm low 64 (upper of a register dst preserved)
        if (d.dst.size == 16) { // load into xmm
            uint64_t scalar = 0;
            if (d.src.kind == OperandKind::Mem) {
                const uint64_t addr = compute_addr(d.src.mem, d.length);
                auto r = mem_.read(addr, &scalar, 8);
                if (!r) return std::unexpected(r.error());
            } else {
                scalar = state_.gpr[d.src.reg & 15];
            }
            dst_xmm.lo = scalar;
            dst_xmm.hi = 0;
            return {};
        }
        const uint64_t low = state_.xmm[d.src.reg & 15].lo;
        if (d.dst.kind == OperandKind::Mem) {
            const uint64_t addr = compute_addr(d.dst.mem, d.length);
            return mem_.write(addr, &low, 8);
        }
        write_reg(state_, d.dst.reg & 15, 8, low);
        return {};
    }
    case SF::QF3: {
        // F3 0F 7E: xmm <- xmm/m64, zero-extends the upper 64 bits.
        if (d.dst.kind == OperandKind::Mem) {
            const uint64_t addr = compute_addr(d.dst.mem, d.length);
            const uint64_t low = state_.xmm[d.src.reg & 15].lo;
            return mem_.write(addr, &low, 8);
        }
        dst_xmm.lo = state_.xmm[d.src.reg & 15].lo;
        dst_xmm.hi = 0;
        return {};
    }
    case SF::QD6: {
        // 66 0F D6: xmm/m64 <- xmm low 64; a register destination keeps its
        // upper 64 bits (store-form semantics).
        if (d.dst.kind == OperandKind::Mem) {
            const uint64_t addr = compute_addr(d.dst.mem, d.length);
            const uint64_t low = state_.xmm[d.src.reg & 15].lo;
            return mem_.write(addr, &low, 8);
        }
        dst_xmm.lo = state_.xmm[d.src.reg & 15].lo;
        return {};
    }
    case SF::PS:
    case SF::PD:
    case SF::DQA:
    case SF::DQU:
    case SF::AP: {
        if (d.dst.kind == OperandKind::Mem) {
            const uint64_t addr = compute_addr(d.dst.mem, d.length);
            return write_mem128(addr, state_.xmm[d.src.reg & 15]);
        }
        if (d.src.kind == OperandKind::Mem) {
            const uint64_t addr = compute_addr(d.src.mem, d.length);
            if ((d.sse_form == SF::DQA || d.sse_form == SF::AP) && (addr % 16) != 0)
                return std::unexpected(
                    Error{Status::GuestFault, "SSE aligned access requires 16-byte alignment"});
            auto r = read_mem128(addr);
            if (!r) return std::unexpected(r.error());
            dst_xmm = *r;
            return {};
        }
        dst_xmm = state_.xmm[d.src.reg & 15];
        return {};
    }
    }
    return std::unexpected(Error{Status::InternalError, "unhandled SSE move form"});
}

std::expected<void, Error> Interpreter::execute_sse_arith(x86::Decoded& d) {
    using SF = SseForm;
    auto srcv = sse_load_source(d);
    if (!srcv) return std::unexpected(srcv.error());
    XmmValue& dst = state_.xmm[d.dst.reg & 15];
    const XmmValue s = *srcv;
    const bool scalar_f32 = d.sse_form == SF::SS;
    const bool scalar_f64 = d.sse_form == SF::SD;
    const bool packed_f32 = d.sse_form == SF::PS;
    const bool packed_f64 = d.sse_form == SF::PD;

    if (d.sse_op == SseOp::And || d.sse_op == SseOp::Or || d.sse_op == SseOp::Xor ||
        d.sse_op == SseOp::Andn) {
        switch (d.sse_op) {
        case SseOp::And: dst.lo &= s.lo; dst.hi &= s.hi; break;
        case SseOp::Or: dst.lo |= s.lo; dst.hi |= s.hi; break;
        case SseOp::Xor: dst.lo ^= s.lo; dst.hi ^= s.hi; break;
        case SseOp::Andn: dst.lo = ~dst.lo & s.lo; dst.hi = ~dst.hi & s.hi; break;
        default: break;
        }
        return {};
    }

    // The form selects exactly one representation: SS/PS operate on f32
    // lanes, SD/PD on f64 lanes. Running both would corrupt lanes with
    // reinterpreted bit patterns.
    const bool f32_form = scalar_f32 || packed_f32;
    auto apply_f32 = [&](auto op) {
        if (scalar_f32) set_lane_f32(dst, 0, op(lane_f32(dst, 0), lane_f32(s, 0)));
        else
            for (int i = 0; i < 4; ++i) set_lane_f32(dst, i, op(lane_f32(dst, i), lane_f32(s, i)));
    };
    auto apply_f64 = [&](auto op) {
        if (scalar_f64) set_lane_f64(dst, 0, op(lane_f64(dst, 0), lane_f64(s, 0)));
        else
            for (int i = 0; i < 2; ++i) set_lane_f64(dst, i, op(lane_f64(dst, i), lane_f64(s, i)));
    };

    switch (d.sse_op) {
    case SseOp::Add:
        if (f32_form) apply_f32(std::plus<float>{});
        else apply_f64(std::plus<double>{});
        break;
    case SseOp::Sub:
        if (f32_form) apply_f32(std::minus<float>{});
        else apply_f64(std::minus<double>{});
        break;
    case SseOp::Mul:
        if (f32_form) apply_f32(std::multiplies<float>{});
        else apply_f64(std::multiplies<double>{});
        break;
    case SseOp::Div:
        if (f32_form) apply_f32(std::divides<float>{});
        else apply_f64(std::divides<double>{});
        break;
    case SseOp::Sqrt: {
        // SQRTSS/SQRTPS are unary: the SOURCE operand is squared-rooted
        // (unlike ADD/SUB/MUL/DIV which are RMW on the destination).
        if (scalar_f32) set_lane_f32(dst, 0, std::sqrt(lane_f32(s, 0)));
        else if (packed_f32)
            for (int i = 0; i < 4; ++i) set_lane_f32(dst, i, std::sqrt(lane_f32(s, i)));
        else if (scalar_f64) set_lane_f64(dst, 0, std::sqrt(lane_f64(s, 0)));
        else if (packed_f64)
            for (int i = 0; i < 2; ++i) set_lane_f64(dst, i, std::sqrt(lane_f64(s, i)));
        break;
    }
    default:
        return std::unexpected(Error{Status::InternalError, "bad SSE arith op"});
    }
    return {};
}

std::expected<void, Error> Interpreter::execute_sse_cmp(x86::Decoded& d) {
    using SF = SseForm;
    auto srcv = sse_load_source(d);
    if (!srcv) return std::unexpected(srcv.error());
    XmmValue& dst = state_.xmm[d.dst.reg & 15];
    const XmmValue s = *srcv;
    const uint8_t pred = d.sse_pred & 7;

    // Predicate evaluator: IEEE comparison result as a bool.
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
    auto cmp_u32 = [&](auto a, auto b, uint8_t p) {
        return cmp_bool(a, b, p) ? 0xFFFFFFFFu : 0u;
    };
    auto cmp_u64 = [&](auto a, auto b, uint8_t p) {
        return cmp_bool(a, b, p) ? ~0ULL : 0ULL;
    };

    if (d.sse_form == SF::SS) {
        set_lane_u32(dst, 0, cmp_u32(lane_f32(dst, 0), lane_f32(s, 0), pred));
    } else if (d.sse_form == SF::PS) {
        for (int i = 0; i < 4; ++i)
            set_lane_u32(dst, i, cmp_u32(lane_f32(dst, i), lane_f32(s, i), pred));
    } else if (d.sse_form == SF::SD) {
        dst.lo = cmp_u64(lane_f64(dst, 0), lane_f64(s, 0), pred);
    } else { // PD
        dst.lo = cmp_u64(lane_f64(dst, 0), lane_f64(s, 0), pred);
        dst.hi = cmp_u64(lane_f64(dst, 1), lane_f64(s, 1), pred);
    }
    return {};
}

std::expected<void, Error> Interpreter::execute_sse_comis(x86::Decoded& d) {
    using SF = SseForm;
    auto srcv = sse_load_source(d);
    if (!srcv) return std::unexpected(srcv.error());
    const XmmValue& a = state_.xmm[d.dst.reg & 15];
    Flags& f = state_.flags;
    f.sf = f.of = f.af = false;
    if (d.sse_form == SF::SS) {
        const float x = lane_f32(a, 0), y = lane_f32(*srcv, 0);
        if (is_nan(x) || is_nan(y)) f.zf = f.pf = f.cf = true;
        else {
            f.zf = x == y;
            f.pf = false;
            f.cf = x < y;
        }
    } else {
        const double x = lane_f64(a, 0), y = lane_f64(*srcv, 0);
        if (is_nan(x) || is_nan(y)) f.zf = f.pf = f.cf = true;
        else {
            f.zf = x == y;
            f.pf = false;
            f.cf = x < y;
        }
    }
    return {};
}

std::expected<void, Error> Interpreter::execute_sse_cvt(x86::Decoded& d) {
    using SF = SseForm;
    auto srcv = sse_load_source(d);
    if (!srcv) return std::unexpected(srcv.error());
    XmmValue& dst = state_.xmm[d.dst.reg & 15];
    const XmmValue s = *srcv;

    switch (d.sse_op) {
    case SseOp::IntToF32:
    case SseOp::IntToF64: {
        // CVTSI2SS/CVTSI2SD: the source is a GPR or memory operand (NOT an
        // xmm), so read it directly instead of through sse_load_source.
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
        if (d.sse_op == SseOp::IntToF32) set_lane_f32(dst, 0, static_cast<float>(iv));
        else set_lane_f64(dst, 0, static_cast<double>(iv));
        return {};
    }
    case SseOp::TruncF32ToI: {
        const float f = lane_f32(s, 0);
        if (is_nan(f) || f >= 2147483648.0f || f < -2147483648.0f) {
            if (d.dst.size == 8) write_reg(state_, d.dst.reg & 15, 8, kIntIndefinite64);
            else write_reg(state_, d.dst.reg & 15, 4, kIntIndefinite32);
            return {};
        }
        const int64_t r = static_cast<int64_t>(f); // truncation toward zero
        if (d.dst.size == 8) write_reg(state_, d.dst.reg & 15, 8, static_cast<uint64_t>(r));
        else write_reg(state_, d.dst.reg & 15, 4, static_cast<uint32_t>(r));
        return {};
    }
    case SseOp::TruncF64ToI: {
        const double v = lane_f64(s, 0);
        if (is_nan(v) || v >= 9223372036854775808.0 || v < -9223372036854775808.0) {
            if (d.dst.size == 8) write_reg(state_, d.dst.reg & 15, 8, kIntIndefinite64);
            else write_reg(state_, d.dst.reg & 15, 4, kIntIndefinite32);
            return {};
        }
        const int64_t r = static_cast<int64_t>(v);
        if (d.dst.size == 8) write_reg(state_, d.dst.reg & 15, 8, static_cast<uint64_t>(r));
        else write_reg(state_, d.dst.reg & 15, 4, static_cast<uint32_t>(r));
        return {};
    }
    case SseOp::RoundF32ToI: {
        const float f = lane_f32(s, 0);
        const double r = std::nearbyint(static_cast<double>(f)); // default: nearest-even
        if (is_nan(f) || r >= 2147483648.0 || r < -2147483648.0) {
            if (d.dst.size == 8) write_reg(state_, d.dst.reg & 15, 8, kIntIndefinite64);
            else write_reg(state_, d.dst.reg & 15, 4, kIntIndefinite32);
            return {};
        }
        if (d.dst.size == 8)
            write_reg(state_, d.dst.reg & 15, 8, static_cast<uint64_t>(static_cast<int64_t>(r)));
        else
            write_reg(state_, d.dst.reg & 15, 4, static_cast<uint32_t>(static_cast<int32_t>(r)));
        return {};
    }
    case SseOp::RoundF64ToI: {
        const double v = lane_f64(s, 0);
        const double r = std::nearbyint(v);
        if (is_nan(v) || r >= 9223372036854775808.0 || r < -9223372036854775808.0) {
            if (d.dst.size == 8) write_reg(state_, d.dst.reg & 15, 8, kIntIndefinite64);
            else write_reg(state_, d.dst.reg & 15, 4, kIntIndefinite32);
            return {};
        }
        if (d.dst.size == 8)
            write_reg(state_, d.dst.reg & 15, 8, static_cast<uint64_t>(static_cast<int64_t>(r)));
        else
            write_reg(state_, d.dst.reg & 15, 4, static_cast<uint32_t>(static_cast<int32_t>(r)));
        return {};
    }
    case SseOp::I32ToF32x4: {
        for (int i = 0; i < 4; ++i)
            set_lane_f32(dst, i, static_cast<float>(static_cast<int32_t>(lane_u32(s, i))));
        return {};
    }
    case SseOp::F32ToI32x4: {
        for (int i = 0; i < 4; ++i) {
            const float f = lane_f32(s, i);
            if (is_nan(f) || f >= 2147483648.0f || f < -2147483648.0f) {
                set_lane_u32(dst, i, kIntIndefinite32);
            } else {
                const int32_t r = static_cast<int32_t>(std::nearbyint(f));
                set_lane_u32(dst, i, static_cast<uint32_t>(r));
            }
        }
        return {};
    }
    case SseOp::TruncF32ToI32x4: {
        for (int i = 0; i < 4; ++i) {
            const float f = lane_f32(s, i);
            if (is_nan(f) || f >= 2147483648.0f || f < -2147483648.0f) {
                set_lane_u32(dst, i, kIntIndefinite32);
            } else {
                set_lane_u32(dst, i, static_cast<uint32_t>(static_cast<int32_t>(f)));
            }
        }
        return {};
    }
    default:
        return std::unexpected(Error{Status::InternalError, "bad SSE cvt op"});
    }
}

std::expected<void, Error> Interpreter::execute_sse_int(x86::Decoded& d) {
    auto srcv = sse_load_source(d);
    if (!srcv) return std::unexpected(srcv.error());
    XmmValue& dst = state_.xmm[d.dst.reg & 15];
    const XmmValue s = *srcv;

    switch (d.sse_op) {
    case SseOp::PaddB: {
        const auto a = std::bit_cast<std::array<uint8_t, 16>>(dst);
        const auto b = std::bit_cast<std::array<uint8_t, 16>>(s);
        std::array<uint8_t, 16> r{};
        for (int i = 0; i < 16; ++i) r[i] = static_cast<uint8_t>(a[i] + b[i]);
        dst = std::bit_cast<XmmValue>(r);
        return {};
    }
    case SseOp::PsubB: {
        const auto a = std::bit_cast<std::array<uint8_t, 16>>(dst);
        const auto b = std::bit_cast<std::array<uint8_t, 16>>(s);
        std::array<uint8_t, 16> r{};
        for (int i = 0; i < 16; ++i) r[i] = static_cast<uint8_t>(a[i] - b[i]);
        dst = std::bit_cast<XmmValue>(r);
        return {};
    }
    case SseOp::PaddW:
    case SseOp::PsubW: {
        uint16_t a[8], b[8];
        std::memcpy(a, &dst, 16);
        std::memcpy(b, &s, 16);
        uint16_t r[8];
        for (int i = 0; i < 8; ++i)
            r[i] = d.sse_op == SseOp::PaddW ? static_cast<uint16_t>(a[i] + b[i])
                                            : static_cast<uint16_t>(a[i] - b[i]);
        std::memcpy(&dst, r, 16);
        return {};
    }
    case SseOp::PaddD:
    case SseOp::PsubD: {
        uint32_t a[4], b[4];
        std::memcpy(a, &dst, 16);
        std::memcpy(b, &s, 16);
        uint32_t r[4];
        for (int i = 0; i < 4; ++i)
            r[i] = d.sse_op == SseOp::PaddD ? static_cast<uint32_t>(a[i] + b[i])
                                            : static_cast<uint32_t>(a[i] - b[i]);
        std::memcpy(&dst, r, 16);
        return {};
    }
    case SseOp::PaddQ:
    case SseOp::PsubQ: {
        if (d.sse_op == SseOp::PaddQ) {
            dst.lo += s.lo;
            dst.hi += s.hi;
        } else {
            dst.lo -= s.lo;
            dst.hi -= s.hi;
        }
        return {};
    }
    case SseOp::Pmullw: {
        int16_t a[8], b[8];
        std::memcpy(a, &dst, 16);
        std::memcpy(b, &s, 16);
        uint16_t r[8];
        for (int i = 0; i < 8; ++i) {
            const int32_t p = static_cast<int32_t>(a[i]) * b[i];
            r[i] = static_cast<uint16_t>(p & 0xFFFF); // low 16 bits
        }
        std::memcpy(&dst, r, 16);
        return {};
    }
    case SseOp::Pmuludq: {
        const uint32_t a0 = lane_u32(dst, 0), a2 = lane_u32(dst, 2);
        const uint32_t b0 = lane_u32(s, 0), b2 = lane_u32(s, 2);
        dst.lo = static_cast<uint64_t>(a0) * b0;
        dst.hi = static_cast<uint64_t>(a2) * b2;
        return {};
    }
    default:
        return std::unexpected(Error{Status::InternalError, "bad SSE int op"});
    }
}

std::expected<void, Error> Interpreter::execute_sse_shuffle(x86::Decoded& d) {
    // PSHUFD: dst dword lane i = src dword lane ((imm >> 2i) & 3)
    auto srcv = sse_load_source(d);
    if (!srcv) return std::unexpected(srcv.error());
    XmmValue& dst = state_.xmm[d.dst.reg & 15];
    const XmmValue s = *srcv;
    const uint8_t imm = static_cast<uint8_t>(d.imm3 & 0xFF);
    XmmValue r{};
    for (int i = 0; i < 4; ++i) set_lane_u32(r, i, lane_u32(s, (imm >> (2 * i)) & 3));
    dst = r;
    return {};
}

} // namespace armsx5::cpu
