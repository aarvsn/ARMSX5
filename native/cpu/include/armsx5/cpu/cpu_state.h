#pragma once
// Guest x86-64 architectural state and register access semantics.
#include <cstdint>
#include <string>

namespace armsx5::cpu {

// Architectural GPR index order.
// 0=AX 1=CX 2=DX 3=BX 4=SP 5=BP 6=SI 7=DI 8..15=R8..R15
namespace reg {
inline constexpr uint8_t AX = 0, CX = 1, DX = 2, BX = 3, SP = 4, BP = 5, SI = 6, DI = 7;
inline constexpr uint8_t R8 = 8, R9 = 9, R10 = 10, R11 = 11, R12 = 12, R13 = 13, R14 = 14,
                        R15 = 15;
} // namespace reg

struct Flags {
    bool cf = false, pf = false, af = false, zf = false, sf = false, of = false, df = false;
    bool operator==(const Flags& o) const {
        return cf == o.cf && pf == o.pf && af == o.af && zf == o.zf && sf == o.sf &&
               of == o.of && df == o.df;
    }
    bool operator!=(const Flags& o) const { return !(*this == o); }
};

struct alignas(16) XmmValue {
    uint64_t lo = 0, hi = 0;
};

// Full 256-bit AVX view: lo = bits 127:0 (the XMM register), hi = bits
// 255:128 (the upper YMM half).
struct Vec256 {
    XmmValue lo{}, hi{};
};

struct CpuState {
    uint64_t gpr[16] = {};
    uint64_t rip = 0;
    Flags flags{};
    XmmValue xmm[16]{};    // XMM0-15 = low 128 bits of YMM0-15
    XmmValue ymm_hi[16]{}; // AVX state: bits 255:128 of YMM0-15

    void reset() { *this = CpuState{}; }
};

// Register access implementing x86-64 partial-write semantics:
//   64-bit write replaces the full register;
//   32-bit write zero-extends into the full register;
//   16/8-bit writes preserve the remaining bits.
// `high_byte` selects AH/CH/DH/BH (bits 8..15 of AX/CX/DX/BX) for 8-bit
// operands encoded without a REX prefix.
uint64_t read_reg(const CpuState& s, uint8_t idx, uint8_t size, bool high_byte = false);
void write_reg(CpuState& s, uint8_t idx, uint8_t size, uint64_t value, bool high_byte = false);

std::string state_summary(const CpuState& s);

} // namespace armsx5::cpu
