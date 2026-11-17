#include "armsx5/cpu/interpreter.h"

#include "armsx5/core/log.h"

#include <array>
#include <cmath>
#include <cstring>

namespace armsx5::cpu {
namespace {
constexpr const char* TAG = "Interpreter";
constexpr size_t kMaxInstrBytes = 15;

struct SizeInfo {
    uint64_t mask;
    uint64_t sign;
    uint8_t bits;
};

SizeInfo size_info(uint8_t size) {
    const uint8_t bits = static_cast<uint8_t>(size * 8);
    const uint64_t mask = size == 8 ? ~0ULL : ((1ULL << bits) - 1);
    const uint64_t sign = 1ULL << (bits - 1);
    return {mask, sign, bits};
}

uint8_t parity(uint64_t v) {
    static std::array<uint8_t, 256> table = [] {
        std::array<uint8_t, 256> t{};
        for (int i = 0; i < 256; ++i) {
            int bits = 0;
            for (int b = 0; b < 8; ++b)
                if (i & (1 << b)) ++bits;
            t[i] = (bits % 2) == 0 ? 1 : 0;
        }
        return t;
    }();
    return table[v & 0xFF];
}

// Sign-based overflow test, exact for add/sub with carry/borrow.
bool add_overflow(uint64_t a, uint64_t b, uint64_t r, uint64_t sign) {
    const bool sa = (a & sign) != 0, sb = (b & sign) != 0, sr = (r & sign) != 0;
    return (sa == sb) && (sa != sr);
}
bool sub_overflow(uint64_t a, uint64_t b, uint64_t r, uint64_t sign) {
    const bool sa = (a & sign) != 0, sb = (b & sign) != 0, sr = (r & sign) != 0;
    return (sa != sb) && (sa != sr);
}

bool cond_true(uint8_t cond, const Flags& f) {
    switch (cond) {
    case 0: return f.of;
    case 1: return !f.of;
    case 2: return f.cf;
    case 3: return !f.cf;
    case 4: return f.zf;
    case 5: return !f.zf;
    case 6: return f.cf || f.zf;
    case 7: return !f.cf && !f.zf;
    case 8: return f.sf;
    case 9: return !f.sf;
    case 10: return f.pf;
    case 11: return !f.pf;
    case 12: return f.sf != f.of;
    case 13: return f.sf == f.of;
    case 14: return f.zf || (f.sf != f.of);
    case 15: return !f.zf && (f.sf == f.of);
    }
    return false;
}

// ---- xmm lane helpers over {lo, hi} halves ----
inline uint32_t lane_u32(const XmmValue& v, int i) {
    return i < 2 ? static_cast<uint32_t>(v.lo >> (32 * i))
                 : static_cast<uint32_t>(v.hi >> (32 * (i - 2)));
}
inline void set_lane_u32(XmmValue& v, int i, uint32_t b) {
    if (i < 2) {
        const uint64_t shift = 32 * i;
        v.lo = (v.lo & ~(0xFFFFFFFFULL << shift)) | (static_cast<uint64_t>(b) << shift);
    } else {
        const uint64_t shift = 32 * (i - 2);
        v.hi = (v.hi & ~(0xFFFFFFFFULL << shift)) | (static_cast<uint64_t>(b) << shift);
    }
}
inline float lane_f32(const XmmValue& v, int i) {
    const uint32_t b = lane_u32(v, i);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
inline void set_lane_f32(XmmValue& v, int i, float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    set_lane_u32(v, i, b);
}
inline double lane_f64(const XmmValue& v, int i) {
    const uint64_t b = i == 0 ? v.lo : v.hi;
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}
inline void set_lane_f64(XmmValue& v, int i, double d) {
    uint64_t b;
    std::memcpy(&b, &d, 8);
    if (i == 0) v.lo = b;
    else v.hi = b;
}

} // namespace

Interpreter::Interpreter(memory::GuestMemory& mem) : mem_(mem) {}

uint64_t Interpreter::compute_addr(const x86::MemOperand& m, uint8_t instr_length) const {
    if (!m.valid) return 0;
    if (m.rip_relative) {
        return state_.rip + instr_length + static_cast<uint64_t>(m.disp);
    }
    uint64_t addr = static_cast<uint64_t>(m.disp); // wraps naturally
    if (m.base != 0xFF) addr += state_.gpr[m.base];
    if (m.index != 0xFF) addr += state_.gpr[m.index] * m.scale;
    return addr;
}

std::expected<uint64_t, Error> Interpreter::read_mem_sized(uint64_t addr, uint8_t size) const {
    uint64_t v = 0;
    auto r = mem_.read(addr, &v, size);
    if (!r) return std::unexpected(r.error());
    return v;
}

std::expected<void, Error> Interpreter::write_mem_sized(uint64_t addr, uint64_t value,
                                                        uint8_t size) {
    return mem_.write(addr, &value, size);
}

std::expected<XmmValue, Error> Interpreter::read_mem128(uint64_t addr) const {
    XmmValue v;
    auto r = mem_.read(addr, &v, 16);
    if (!r) return std::unexpected(r.error());
    return v;
}

std::expected<void, Error> Interpreter::write_mem128(uint64_t addr, const XmmValue& v) {
    return mem_.write(addr, &v, 16);
}

Vec256 Interpreter::read_vec(uint8_t reg, bool wide) const {
    Vec256 v;
    v.lo = state_.xmm[reg & 15];
    if (wide) v.hi = state_.ymm_hi[reg & 15];
    return v;
}

void Interpreter::write_vec(uint8_t reg, bool wide, const Vec256& v) {
    state_.xmm[reg & 15] = v.lo;
    if (wide) {
        state_.ymm_hi[reg & 15] = v.hi;
    } else {
        // VEX.128 destination rule: zero the upper YMM half (hardware-verified).
        state_.ymm_hi[reg & 15] = XmmValue{};
    }
}

std::expected<Vec256, Error> Interpreter::read_mem256(uint64_t addr) const {
    Vec256 v;
    auto r = mem_.read(addr, &v, 32);
    if (!r) return std::unexpected(r.error());
    return v;
}

std::expected<void, Error> Interpreter::write_mem256(uint64_t addr, const Vec256& v) {
    return mem_.write(addr, &v, 32);
}

std::expected<uint64_t, Error> Interpreter::read_operand(const x86::Operand& op,
                                                         const x86::Decoded& d) const {
    switch (op.kind) {
    case x86::OperandKind::None:
        return 0ULL;
    case x86::OperandKind::Reg:
        return read_reg(state_, op.reg, op.size, op.high_byte);
    case x86::OperandKind::Mem: {
        const uint64_t addr = compute_addr(op.mem, d.length);
        return read_mem_sized(addr, op.size);
    }
    case x86::OperandKind::Imm: {
        const SizeInfo si = size_info(op.size);
        return op.imm & si.mask;
    }
    }
    return std::unexpected(Error{Status::InternalError, "bad operand kind"});
}

std::expected<void, Error> Interpreter::write_operand(const x86::Operand& op, uint64_t value,
                                                      const x86::Decoded& d) {
    switch (op.kind) {
    case x86::OperandKind::Reg:
        write_reg(state_, op.reg, op.size, value, op.high_byte);
        return {};
    case x86::OperandKind::Mem: {
        const uint64_t addr = compute_addr(op.mem, d.length);
        return write_mem_sized(addr, value, op.size);
    }
    default:
        return std::unexpected(Error{Status::InternalError, "cannot write to this operand"});
    }
}

std::expected<void, Error> Interpreter::push(uint64_t value, uint8_t size) {
    const uint64_t rsp = state_.gpr[reg::SP] - size;
    state_.gpr[reg::SP] = rsp;
    return write_mem_sized(rsp, value, size);
}

std::expected<uint64_t, Error> Interpreter::pop(uint8_t size) {
    auto v = read_mem_sized(state_.gpr[reg::SP], size);
    if (!v) return v;
    state_.gpr[reg::SP] += size;
    return v;
}

std::expected<void, Error> Interpreter::execute(x86::Decoded& d) {
    const SizeInfo si = size_info(d.opsize);
    using x86::InstrClass;
    using x86::OperandKind;

    switch (d.cls) {
    case InstrClass::Nop:
        return {};
    case InstrClass::SseMov:
        return d.vex ? execute_avx_mov(d) : execute_sse_mov(d);
    case InstrClass::SseArith:
        return d.vex ? execute_avx_arith(d) : execute_sse_arith(d);
    case InstrClass::SseCmp:
        return d.vex ? execute_avx_cmp(d) : execute_sse_cmp(d);
    case InstrClass::SseComis:
        return d.vex ? execute_avx_comis(d) : execute_sse_comis(d);
    case InstrClass::SseCvt:
        return d.vex ? execute_avx_cvt(d) : execute_sse_cvt(d);
    case InstrClass::SseInt:
        return d.vex ? execute_avx_int(d) : execute_sse_int(d);
    case InstrClass::SseShuffle:
        return d.vex ? execute_avx_shuffle(d) : execute_sse_shuffle(d);

    case InstrClass::Mov: {
        auto v = read_operand(d.src, d);
        if (!v) return std::unexpected(v.error());
        return write_operand(d.dst, *v & si.mask, d);
    }
    case InstrClass::MovImm: {
        return write_operand(d.dst, static_cast<uint64_t>(d.src.imm) & si.mask, d);
    }
    case InstrClass::Lea: {
        if (d.src.kind != OperandKind::Mem)
            return std::unexpected(Error{Status::InternalError, "LEA without memory operand"});
        const uint64_t addr = compute_addr(d.src.mem, d.length);
        return write_operand(d.dst, addr & si.mask, d);
    }

    case InstrClass::Alu: {
        auto a = read_operand(d.dst, d);
        if (!a) return std::unexpected(a.error());
        uint64_t b = 0;
        if (d.src.kind == OperandKind::Imm) {
            b = static_cast<uint64_t>(d.src.imm) & si.mask;
        } else {
            auto bv = read_operand(d.src, d);
            if (!bv) return std::unexpected(bv.error());
            b = *bv;
        }
        using x86::AluOp;
        uint64_t r = 0;
        switch (d.alu_op) {
        case AluOp::Add:
        case AluOp::Adc: {
            const unsigned cin = d.alu_op == AluOp::Adc ? (state_.flags.cf ? 1 : 0) : 0;
            const __uint128_t t = static_cast<__uint128_t>(*a) + b + cin;
            r = static_cast<uint64_t>(t) & si.mask;
            state_.flags.cf = (t >> si.bits) != 0;
            state_.flags.of = add_overflow(*a, b, r, si.sign);
            state_.flags.af = ((*a ^ b ^ r) & 0x10) != 0;
            state_.flags.zf = r == 0;
            state_.flags.sf = (r & si.sign) != 0;
            state_.flags.pf = parity(r);
            break;
        }
        case AluOp::Sub:
        case AluOp::Sbb:
        case AluOp::Cmp: {
            const unsigned bin = d.alu_op == AluOp::Sbb ? (state_.flags.cf ? 1 : 0) : 0;
            const __int128_t t = static_cast<__int128_t>(*a) - b - bin;
            r = static_cast<uint64_t>(t) & si.mask;
            state_.flags.cf = t < 0;
            state_.flags.of = sub_overflow(*a, b, r, si.sign);
            state_.flags.af = ((*a ^ b ^ r) & 0x10) != 0;
            state_.flags.zf = r == 0;
            state_.flags.sf = (r & si.sign) != 0;
            state_.flags.pf = parity(r);
            if (d.alu_op == AluOp::Cmp) return {}; // result discarded
            break;
        }
        case AluOp::And:
        case AluOp::Or:
        case AluOp::Xor: {
            if (d.alu_op == AluOp::And) r = *a & b;
            else if (d.alu_op == AluOp::Or) r = *a | b;
            else r = *a ^ b;
            state_.flags.cf = false;
            state_.flags.of = false;
            state_.flags.af = false;
            state_.flags.zf = r == 0;
            state_.flags.sf = (r & si.sign) != 0;
            state_.flags.pf = parity(r);
            break;
        }
        }
        return write_operand(d.dst, r, d);
    }

    case InstrClass::Test: {
        auto a = read_operand(d.dst, d);
        if (!a) return std::unexpected(a.error());
        auto b = read_operand(d.src, d);
        if (!b) return std::unexpected(b.error());
        const uint64_t r = *a & *b;
        state_.flags.cf = false;
        state_.flags.of = false;
        state_.flags.af = false;
        state_.flags.zf = r == 0;
        state_.flags.sf = (r & si.sign) != 0;
        state_.flags.pf = parity(r);
        return {};
    }

    case InstrClass::Push: {
        uint64_t v;
        if (d.src.kind == OperandKind::Imm) {
            v = static_cast<uint64_t>(d.src.imm);
        } else {
            auto r = read_operand(d.dst, d);
            if (!r) return std::unexpected(r.error());
            v = *r;
        }
        auto w = push(v, d.opsize == 2 ? 2 : 8);
        if (!w) return w;
        return {};
    }
    case InstrClass::Pop: {
        auto v = pop(8);
        if (!v) return std::unexpected(v.error());
        return write_operand(d.dst, *v, d);
    }

    case InstrClass::CallRel: {
        const uint64_t next_rip = state_.rip + d.length;
        auto r = push(next_rip, 8);
        if (!r) return r;
        state_.rip = next_rip + static_cast<uint64_t>(d.rel);
        return {};
    }
    case InstrClass::Call: {
        auto t = read_operand(d.dst, d);
        if (!t) return std::unexpected(t.error());
        auto r = push(state_.rip + d.length, 8);
        if (!r) return r;
        state_.rip = *t;
        return {};
    }
    case InstrClass::Ret: {
        auto v = pop(8);
        if (!v) return std::unexpected(v.error());
        state_.rip = *v;
        if (d.src.imm != 0) state_.gpr[reg::SP] += static_cast<uint64_t>(d.src.imm);
        return {};
    }
    case InstrClass::JmpRel: {
        // Relative targets resolve from the END of the instruction.
        state_.rip = state_.rip + static_cast<uint64_t>(d.rel) + d.length;
        return {};
    }
    case InstrClass::JmpReg: {
        auto t = read_operand(d.dst, d);
        if (!t) return std::unexpected(t.error());
        state_.rip = *t;
        return {};
    }
    case InstrClass::Jcc: {
        if (cond_true(d.cond, state_.flags)) {
            // Relative targets resolve from the END of the instruction.
            state_.rip = state_.rip + static_cast<uint64_t>(d.rel) + d.length;
        } else {
            state_.rip += d.length;
        }
        return {};
    }

    case InstrClass::Setcc: {
        return write_operand(d.dst, cond_true(d.cond, state_.flags) ? 1 : 0, d);
    }
    case InstrClass::Cmovcc: {
        auto v = read_operand(d.src, d);
        if (!v) return std::unexpected(v.error());
        if (cond_true(d.cond, state_.flags)) return write_operand(d.dst, *v & si.mask, d);
        return {};
    }

    case InstrClass::Shift: {
        auto a = read_operand(d.dst, d);
        if (!a) return std::unexpected(a.error());
        uint64_t count = 0;
        if (d.src.kind == OperandKind::Imm) {
            count = static_cast<uint64_t>(d.src.imm);
        } else {
            count = read_reg(state_, reg::CX, 1, false);
        }
        count &= d.opsize == 8 ? 0x3F : 0x1F;
        if (count == 0) return {}; // no flag or value change
        uint64_t r = 0;
        const uint64_t bits = si.bits;
        using x86::ShiftOp;
        switch (d.shift_op) {
        case ShiftOp::Shl:
        case ShiftOp::Sal: {
            if (count > bits) {
                r = 0;
                state_.flags.cf = false;
            } else {
                r = (*a << count) & si.mask;
                state_.flags.cf = ((*a >> (bits - count)) & 1) != 0;
            }
            state_.flags.of = count == 1
                                  ? (((r & si.sign) != 0) != state_.flags.cf)
                                  : state_.flags.of;
            break;
        }
        case ShiftOp::Shr: {
            if (count > bits) {
                r = 0;
                state_.flags.cf = false;
            } else {
                r = (*a >> count) & si.mask;
                state_.flags.cf = ((*a >> (count - 1)) & 1) != 0;
            }
            state_.flags.of = count == 1 ? ((*a & si.sign) != 0) : state_.flags.of;
            break;
        }
        case ShiftOp::Sar: {
            const int64_t sa = static_cast<int64_t>(*a);
            if (count > bits) {
                r = (sa < 0) ? si.mask : 0;
                state_.flags.cf = (sa < 0);
            } else {
                r = static_cast<uint64_t>(sa >> count) & si.mask;
                state_.flags.cf = (static_cast<int64_t>(*a) >> (count - 1)) & 1;
            }
            state_.flags.of = false;
            break;
        }
        case ShiftOp::Rol: {
            const uint64_t eff = count % bits;
            if (eff == 0) return {};
            r = ((*a << eff) | (*a >> (bits - eff))) & si.mask;
            state_.flags.cf = r & 1;
            state_.flags.of = eff == 1 ? (((r & si.sign) != 0) != (state_.flags.cf != 0))
                                       : state_.flags.of;
            break;
        }
        case ShiftOp::Ror: {
            const uint64_t eff = count % bits;
            if (eff == 0) return {};
            r = ((*a >> eff) | (*a << (bits - eff))) & si.mask;
            state_.flags.cf = (r & si.sign) != 0;
            state_.flags.of = eff == 1
                                  ? (((r & si.sign) != 0) != (((r >> (si.bits - 2)) & 1) != 0))
                                  : state_.flags.of;
            break;
        }
        default:
            return std::unexpected(Error{Status::Unsupported, "shift op not implemented"});
        }
        // SF/ZF/AF/PF are modified by count != 0 for non-rotate shifts only.
        if (d.shift_op == ShiftOp::Shl || d.shift_op == ShiftOp::Sal ||
            d.shift_op == ShiftOp::Shr || d.shift_op == ShiftOp::Sar) {
            state_.flags.zf = r == 0;
            state_.flags.sf = (r & si.sign) != 0;
            state_.flags.pf = parity(r);
            state_.flags.af = false;
        }
        return write_operand(d.dst, r, d);
    }

    case InstrClass::Mul: {
        auto v = read_operand(d.dst, d);
        if (!v) return std::unexpected(v.error());
        if (d.opsize == 1) {
            const uint64_t al = state_.gpr[reg::AX] & 0xFF;
            const uint64_t full = al * (*v & 0xFF);
            state_.gpr[reg::AX] = (state_.gpr[reg::AX] & ~0xFFFFULL) | full;
            state_.flags.cf = state_.flags.of = (full >> 8) != 0;
        } else if (d.opsize == 2) {
            const uint64_t ax = state_.gpr[reg::AX] & 0xFFFF;
            const uint64_t full = ax * (*v & 0xFFFF);
            state_.gpr[reg::AX] = (state_.gpr[reg::AX] & ~0xFFFFULL) | (full & 0xFFFF);
            state_.gpr[reg::DX] = (state_.gpr[reg::DX] & ~0xFFFFULL) | (full >> 16);
            state_.flags.cf = state_.flags.of = (full >> 16) != 0;
        } else if (d.opsize == 4) {
            const uint64_t eax = state_.gpr[reg::AX] & 0xFFFFFFFF;
            const uint64_t full = eax * (*v & 0xFFFFFFFF);
            write_reg(state_, reg::AX, 4, full & 0xFFFFFFFF);
            write_reg(state_, reg::DX, 4, (full >> 32) & 0xFFFFFFFF);
            state_.flags.cf = state_.flags.of = (full >> 32) != 0;
        } else {
            const __uint128_t full =
                static_cast<__uint128_t>(state_.gpr[reg::AX]) * static_cast<__uint128_t>(*v);
            state_.gpr[reg::AX] = static_cast<uint64_t>(full);
            state_.gpr[reg::DX] = static_cast<uint64_t>(full >> 64);
            state_.flags.cf = state_.flags.of = (full >> 64) != 0;
        }
        return {};
    }

    case InstrClass::IMul: {
        // Forms:
        //   0: one-operand  (GRP3 /5): rDX:rAX = sign(rAX * r/m)
        //   1: two-operand  (0F AF):   r = r * r/m
        //   2: three-operand (69/6B):  r = r/m * imm
        if (d.imul_form == 0) {
            auto v = read_operand(d.dst, d);
            if (!v) return std::unexpected(v.error());
            if (d.opsize == 8) {
                const __int128_t f = static_cast<__int128_t>(static_cast<int64_t>(state_.gpr[reg::AX])) *
                                     static_cast<__int128_t>(static_cast<int64_t>(*v));
                state_.gpr[reg::AX] = static_cast<uint64_t>(f);
                state_.gpr[reg::DX] = static_cast<uint64_t>(f >> 64);
                const int64_t low = static_cast<int64_t>(state_.gpr[reg::AX]);
                state_.flags.cf = state_.flags.of =
                    f != static_cast<__int128_t>(static_cast<int64_t>(low));
                return {};
            }
            int64_t full = 0;
            if (d.opsize == 1) {
                const int64_t al = static_cast<int8_t>(state_.gpr[reg::AX] & 0xFF);
                full = al * static_cast<int8_t>(*v & 0xFF);
                state_.gpr[reg::AX] = (state_.gpr[reg::AX] & ~0xFFFFULL) |
                                      (static_cast<uint64_t>(static_cast<int16_t>(full)) & 0xFFFF);
            } else if (d.opsize == 2) {
                const int64_t ax = static_cast<int16_t>(state_.gpr[reg::AX] & 0xFFFF);
                full = ax * static_cast<int16_t>(*v & 0xFFFF);
                state_.gpr[reg::AX] = (state_.gpr[reg::AX] & ~0xFFFFULL) |
                                      (static_cast<uint64_t>(full) & 0xFFFF);
                state_.gpr[reg::DX] = (state_.gpr[reg::DX] & ~0xFFFFULL) |
                                      ((static_cast<uint64_t>(full) >> 16) & 0xFFFF);
            } else { // 4
                const int64_t eax = static_cast<int32_t>(state_.gpr[reg::AX]);
                full = eax * static_cast<int32_t>(*v);
                write_reg(state_, reg::AX, 4, static_cast<uint64_t>(full) & 0xFFFFFFFF);
                write_reg(state_, reg::DX, 4, (static_cast<uint64_t>(full) >> 32) & 0xFFFFFFFF);
            }
            const int64_t low = d.opsize == 1 ? static_cast<int8_t>(full)
                                : d.opsize == 2 ? static_cast<int16_t>(full)
                                : d.opsize == 4 ? static_cast<int32_t>(full)
                                                : full;
            state_.flags.cf = state_.flags.of = full != low;
            return {};
        }

        uint64_t a;
        int64_t b;
        if (d.imul_form == 2) {
            auto av = read_operand(d.src, d);
            if (!av) return std::unexpected(av.error());
            a = *av;
            b = d.imm3;
        } else {
            auto av = read_operand(d.dst, d);
            if (!av) return std::unexpected(av.error());
            a = *av;
            auto bv = read_operand(d.src, d);
            if (!bv) return std::unexpected(bv.error());
            b = static_cast<int64_t>(*bv);
        }
        int64_t sa = static_cast<int64_t>(a);
        if (d.opsize == 4) sa = static_cast<int32_t>(sa);
        else if (d.opsize == 2) sa = static_cast<int16_t>(sa);
        else if (d.opsize == 1) sa = static_cast<int8_t>(sa);
        const int64_t full = sa * b;
        const uint64_t r = static_cast<uint64_t>(full) & si.mask;
        const int64_t trunc = d.opsize == 8 ? full
                              : d.opsize == 4 ? static_cast<int32_t>(full)
                              : d.opsize == 2 ? static_cast<int16_t>(full)
                                              : static_cast<int8_t>(full);
        state_.flags.cf = state_.flags.of = full != trunc;
        state_.flags.zf = (r == 0);
        state_.flags.sf = (r & si.sign) != 0;
        state_.flags.pf = parity(r);
        return write_operand(d.dst, r, d);
    }

    case InstrClass::Div: {
        auto v = read_operand(d.dst, d);
        if (!v) return std::unexpected(v.error());
        const bool signed_div = d.cond == 1;
        if (d.opsize == 1) {
            const uint64_t den = *v & 0xFF;
            if (den == 0)
                return std::unexpected(Error{Status::GuestFault, "#DE: division by zero"});
            if (!signed_div) {
                const uint64_t num = state_.gpr[reg::AX] & 0xFFFF;
                const uint64_t q = num / den, r = num % den;
                if (q > 0xFF)
                    return std::unexpected(Error{Status::GuestFault, "#DE: divide overflow"});
                state_.gpr[reg::AX] = (state_.gpr[reg::AX] & ~0xFFFFULL) | (q & 0xFF) |
                                      ((r & 0xFF) << 8);
            } else {
                const int64_t num = static_cast<int16_t>(state_.gpr[reg::AX] & 0xFFFF);
                const int64_t dn = static_cast<int8_t>(den);
                if (dn == 0)
                    return std::unexpected(Error{Status::GuestFault, "#DE: division by zero"});
                const int64_t q = num / dn, r = num % dn;
                if (q > 0x7F || q < -0x80)
                    return std::unexpected(Error{Status::GuestFault, "#DE: divide overflow"});
                state_.gpr[reg::AX] = (state_.gpr[reg::AX] & ~0xFFFFULL) |
                                      ((static_cast<uint64_t>(q) & 0xFF)) |
                                      ((static_cast<uint64_t>(r) & 0xFF) << 8);
            }
            return {};
        }
        const uint64_t den = *v & si.mask;
        if (den == 0)
            return std::unexpected(Error{Status::GuestFault, "#DE: division by zero"});
        if (d.opsize == 8) {
            if (!signed_div) {
                const __uint128_t num = (static_cast<__uint128_t>(state_.gpr[reg::DX]) << 64) |
                                        state_.gpr[reg::AX];
                const __uint128_t q = num / den;
                const __uint128_t r = num % den;
                if (q > ~0ULL)
                    return std::unexpected(Error{Status::GuestFault, "#DE: divide overflow"});
                state_.gpr[reg::AX] = static_cast<uint64_t>(q);
                state_.gpr[reg::DX] = static_cast<uint64_t>(r);
            } else {
                const __int128_t num =
                    (static_cast<__int128_t>(static_cast<int64_t>(state_.gpr[reg::DX])) << 64) |
                    state_.gpr[reg::AX];
                const __int128_t dn = static_cast<__int128_t>(static_cast<int64_t>(den));
                if (dn == 0)
                    return std::unexpected(Error{Status::GuestFault, "#DE: division by zero"});
                const __int128_t q = num / dn, r = num % dn;
                if (q > INT64_MAX || q < INT64_MIN)
                    return std::unexpected(Error{Status::GuestFault, "#DE: divide overflow"});
                state_.gpr[reg::AX] = static_cast<uint64_t>(static_cast<int64_t>(q));
                state_.gpr[reg::DX] = static_cast<uint64_t>(static_cast<int64_t>(r));
            }
        } else if (d.opsize == 4) {
            const uint64_t hi = state_.gpr[reg::DX] & 0xFFFFFFFF;
            const uint64_t lo = state_.gpr[reg::AX] & 0xFFFFFFFF;
            if (!signed_div) {
                const uint64_t num = (hi << 32) | lo;
                const uint64_t q = num / den, r = num % den;
                if (q > 0xFFFFFFFF)
                    return std::unexpected(Error{Status::GuestFault, "#DE: divide overflow"});
                write_reg(state_, reg::AX, 4, q);
                write_reg(state_, reg::DX, 4, r);
            } else {
                const int64_t num = static_cast<int64_t>(
                    (static_cast<uint64_t>(static_cast<int32_t>(hi)) << 32) | lo);
                const int64_t dn = static_cast<int32_t>(den);
                const int64_t q = num / dn, r = num % dn;
                if (q > INT32_MAX || q < INT32_MIN)
                    return std::unexpected(Error{Status::GuestFault, "#DE: divide overflow"});
                write_reg(state_, reg::AX, 4, static_cast<uint64_t>(q) & 0xFFFFFFFF);
                write_reg(state_, reg::DX, 4, static_cast<uint64_t>(r) & 0xFFFFFFFF);
            }
        } else { // 16-bit
            const uint64_t num = ((state_.gpr[reg::DX] & 0xFFFF) << 16) |
                                 (state_.gpr[reg::AX] & 0xFFFF);
            if (!signed_div) {
                const uint64_t q = num / den, r = num % den;
                if (q > 0xFFFF)
                    return std::unexpected(Error{Status::GuestFault, "#DE: divide overflow"});
                state_.gpr[reg::AX] = (state_.gpr[reg::AX] & ~0xFFFFULL) | q;
                state_.gpr[reg::DX] = (state_.gpr[reg::DX] & ~0xFFFFULL) | r;
            } else {
                const int64_t num_s = static_cast<int32_t>(num);
                const int64_t dn = static_cast<int16_t>(den);
                const int64_t q = num_s / dn, r = num_s % dn;
                if (q > 0x7FFF || q < -0x8000)
                    return std::unexpected(Error{Status::GuestFault, "#DE: divide overflow"});
                state_.gpr[reg::AX] = (state_.gpr[reg::AX] & ~0xFFFFULL) |
                                      (static_cast<uint64_t>(q) & 0xFFFF);
                state_.gpr[reg::DX] = (state_.gpr[reg::DX] & ~0xFFFFULL) |
                                      (static_cast<uint64_t>(r) & 0xFFFF);
            }
        }
        return {};
    }

    case InstrClass::Neg: {
        auto a = read_operand(d.dst, d);
        if (!a) return std::unexpected(a.error());
        const __int128_t t = -static_cast<__int128_t>(*a);
        const uint64_t r = static_cast<uint64_t>(t) & si.mask;
        state_.flags.cf = (*a & si.mask) != 0;
        state_.flags.of = sub_overflow(0, *a & si.mask, r, si.sign);
        state_.flags.af = ((0 ^ *a ^ r) & 0x10) != 0;
        state_.flags.zf = r == 0;
        state_.flags.sf = (r & si.sign) != 0;
        state_.flags.pf = parity(r);
        return write_operand(d.dst, r, d);
    }
    case InstrClass::Not: {
        auto a = read_operand(d.dst, d);
        if (!a) return std::unexpected(a.error());
        return write_operand(d.dst, (~*a) & si.mask, d);
    }
    case InstrClass::IncDec: {
        auto a = read_operand(d.dst, d);
        if (!a) return std::unexpected(a.error());
        const bool saved_cf = state_.flags.cf;
        uint64_t r;
        if (d.cond == 0) { // INC
            const __uint128_t t = *a + 1;
            r = static_cast<uint64_t>(t) & si.mask;
            state_.flags.of = add_overflow(*a, 1, r, si.sign);
            state_.flags.af = ((*a ^ 1 ^ r) & 0x10) != 0;
        } else { // DEC
            const __int128_t t = static_cast<__int128_t>(*a) - 1;
            r = static_cast<uint64_t>(t) & si.mask;
            state_.flags.of = sub_overflow(*a, 1, r, si.sign);
            state_.flags.af = ((*a ^ 1 ^ r) & 0x10) != 0;
        }
        state_.flags.cf = saved_cf;
        state_.flags.zf = r == 0;
        state_.flags.sf = (r & si.sign) != 0;
        state_.flags.pf = parity(r);
        return write_operand(d.dst, r, d);
    }

    case InstrClass::Movzx: {
        auto v = read_operand(d.src, d);
        if (!v) return std::unexpected(v.error());
        return write_operand(d.dst, *v, d);
    }
    case InstrClass::Movsx:
    case InstrClass::Movsxd: {
        auto v = read_operand(d.src, d);
        if (!v) return std::unexpected(v.error());
        uint64_t r = *v;
        if (d.cls == InstrClass::Movsx) {
            const SizeInfo ssi = size_info(d.src.size);
            r = (*v & ssi.sign) ? (*v | ~ssi.mask) : *v;
        } else {
            r = (*v & 0x80000000ULL) ? (*v | 0xFFFFFFFF00000000ULL) : *v;
        }
        return write_operand(d.dst, r, d);
    }

    case InstrClass::Xchg: {
        auto a = read_operand(d.dst, d);
        if (!a) return std::unexpected(a.error());
        auto b = read_operand(d.src, d);
        if (!b) return std::unexpected(b.error());
        auto w1 = write_operand(d.dst, *b & si.mask, d);
        if (!w1) return w1;
        return write_operand(d.src, *a & si.mask, d);
    }

    case InstrClass::SignExtend: {
        if (d.cond == 1) { // CDQ/CQO: sign-extend into RDX/EDX
            if (d.opsize == 8) state_.gpr[reg::DX] = (state_.gpr[reg::AX] >> 63) ? ~0ULL : 0;
            else write_reg(state_, reg::DX, 4, (state_.gpr[reg::AX] >> 31) ? 0xFFFFFFFF : 0);
            return {};
        }
        if (d.opsize == 8) { // CDQE
            const uint64_t r = (state_.gpr[reg::AX] & 0x80000000ULL)
                                   ? (state_.gpr[reg::AX] | 0xFFFFFFFF00000000ULL)
                                   : (state_.gpr[reg::AX] & 0xFFFFFFFF);
            state_.gpr[reg::AX] = r;
        } else { // CWDE
            const uint64_t r = (state_.gpr[reg::AX] & 0x8000ULL)
                                   ? ((state_.gpr[reg::AX] & 0xFFFF) | 0xFFFF0000ULL)
                                   : (state_.gpr[reg::AX] & 0xFFFF);
            write_reg(state_, reg::AX, 4, r);
        }
        return {};
    }

    case InstrClass::Syscall: {
        state_.rip += d.length;
        if (!syscall_)
            return std::unexpected(Error{Status::KernelError, "no syscall handler installed"});
        bool stop = false;
        const Status r = syscall_->on_syscall(state_, stop);
        if (r != Status::Ok)
            return std::unexpected(Error{r, "syscall handler failed"});
        if (stop) run_stop_ = true;
        return {};
    }

    case InstrClass::Invalid:
    default:
        return std::unexpected(Error{Status::InternalError, "decoded to invalid class"});
    }
}

std::expected<void, Error> Interpreter::step() {
    const uint64_t start_rip = state_.rip;

    // Fetch instruction bytes. Reads may stop early at a mapping boundary;
    // decoding an instruction that crosses that boundary then fails cleanly.
    uint8_t buf[kMaxInstrBytes] = {};
    size_t filled = 0;
    for (; filled < kMaxInstrBytes; ++filled) {
        uint8_t b = 0;
        auto r = mem_.read(start_rip + filled, &b, 1);
        if (!r) break;
        buf[filled] = b;
    }
    if (filled == 0)
        return std::unexpected(Error{Status::GuestFault, "cannot fetch instruction: unmapped RIP"});

    auto d = x86::decode(buf, filled);
    if (!d) return std::unexpected(d.error());
    x86::Decoded dec = *d;
    ARMSX5_LOG_TRACE(TAG, "rip=0x%llx %s len=%u",
                     static_cast<unsigned long long>(start_rip),
                     x86::class_name(dec.cls), dec.length);

    auto r = execute(dec);
    if (!r) {
        // RIP stays at the faulting instruction for diagnostics.
        state_.rip = start_rip;
        return std::unexpected(r.error());
    }
    // Control-flow classes update RIP inside execute(); everything else
    // advances to the next instruction.
    if (dec.cls != x86::InstrClass::Jcc && dec.cls != x86::InstrClass::JmpRel &&
        dec.cls != x86::InstrClass::JmpReg && dec.cls != x86::InstrClass::Ret &&
        dec.cls != x86::InstrClass::CallRel && dec.cls != x86::InstrClass::Call &&
        dec.cls != x86::InstrClass::Syscall) {
        state_.rip = start_rip + dec.length;
    }
    return {};
}

RunStats Interpreter::run(uint64_t max_steps) {
    RunStats stats;
    run_stop_ = false;
    for (uint64_t i = 0; i < max_steps; ++i) {
        const uint64_t rip_before = state_.rip;
        auto r = step();
        ++stats.steps;
        if (!r) {
            if (r.error().code == Status::Unsupported) {
                stats.stop = RunStats::Stop::Unsupported;
                stats.status = Status::Unsupported;
                stats.fault_rip = rip_before;
                stats.message = r.error().message;
            } else {
                stats.stop = RunStats::Stop::Fault;
                stats.status = r.error().code;
                stats.fault_rip = rip_before;
                stats.message = r.error().message;
            }
            return stats;
        }
        if (run_stop_) {
            stats.stop = RunStats::Stop::Exited;
            stats.exit_code = state_.gpr[reg::AX];
            stats.status = Status::Ok;
            return stats;
        }
    }
    stats.stop = RunStats::Stop::MaxSteps;
    stats.status = Status::Ok;
    return stats;
}

const char* Interpreter::stop_name(RunStats::Stop stop) {
    switch (stop) {
    case RunStats::Stop::Exited: return "Exited";
    case RunStats::Stop::MaxSteps: return "MaxSteps";
    case RunStats::Stop::Fault: return "Fault";
    case RunStats::Stop::Unsupported: return "Unsupported";
    }
    return "?";
}

uint64_t read_reg(const CpuState& s, uint8_t idx, uint8_t size, bool high_byte) {
    const uint64_t v = s.gpr[idx & 15];
    switch (size) {
    case 1:
        return high_byte ? (v >> 8) & 0xFF : v & 0xFF;
    case 2: return v & 0xFFFF;
    case 4: return v & 0xFFFFFFFF;
    case 8: return v;
    }
    return v;
}

void write_reg(CpuState& s, uint8_t idx, uint8_t size, uint64_t value, bool high_byte) {
    uint64_t& reg = s.gpr[idx & 15];
    switch (size) {
    case 1:
        if (high_byte) {
            reg = (reg & ~0xFF00ULL) | ((value & 0xFF) << 8);
        } else {
            reg = (reg & ~0xFFULL) | (value & 0xFF);
        }
        break;
    case 2: reg = (reg & ~0xFFFFULL) | (value & 0xFFFF); break;
    case 4: reg = value & 0xFFFFFFFF; break; // zero-extend
    case 8: reg = value; break;
    default: break;
    }
}

std::string state_summary(const CpuState& s) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "rip=0x%016llx rax=0x%016llx rbx=0x%016llx rcx=0x%016llx rdx=0x%016llx rsp=0x%016llx",
             static_cast<unsigned long long>(s.rip), static_cast<unsigned long long>(s.gpr[0]),
             static_cast<unsigned long long>(s.gpr[3]), static_cast<unsigned long long>(s.gpr[1]),
             static_cast<unsigned long long>(s.gpr[2]), static_cast<unsigned long long>(s.gpr[4]));
    return buf;
}

} // namespace armsx5::cpu
