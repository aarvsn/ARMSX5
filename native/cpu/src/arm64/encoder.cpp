#include "armsx5/cpu/arm64/encoder.h"

#include <cassert>

namespace armsx5::cpu::arm64 {

Cond cond_from_x86(uint8_t c) {
    static constexpr Cond kMap[16] = {
        VS,  // x86 O  -> ARM VS
        VC,  // x86 NO -> ARM VC
        CS,  // x86 B  -> ARM CS
        CC,  // x86 NB -> ARM CC
        EQ,  // x86 E  -> ARM EQ
        NE,  // x86 NE -> ARM NE
        LS,  // x86 BE -> ARM LS
        HI,  // x86 NBE-> ARM HI
        MI,  // x86 S  -> ARM MI
        PL,  // x86 NS -> ARM PL
        VS,  // x86 P  -> ARM VS (parity treated as float-unordered slot in fast paths)
        VC,  // x86 NP -> ARM VC
        LT,  // x86 L  -> ARM LT
        GE,  // x86 NL -> ARM GE
        LE,  // x86 LE -> ARM LE
        GT,  // x86 G  -> ARM GT
    };
    return kMap[c & 15];
}

int Assembler::new_label() {
    label_pos_.push_back(0); // 0 = unbound sentinel
    return static_cast<int>(label_pos_.size()) - 1;
}

void Assembler::bind(int label) {
    assert(label >= 0 && label < static_cast<int>(label_pos_.size()));
    label_pos_[label] = code_.size() + 1; // +1 keeps 0 as unbound sentinel
}

bool Assembler::finalize() {
    for (const Fixup& f : fixups_) {
        if (f.label < 0 || f.label >= static_cast<int>(label_pos_.size())) return false;
        const size_t target = label_pos_[f.label];
        if (target == 0) return false; // unbound label
        const int64_t diff = static_cast<int64_t>(target - 1) - static_cast<int64_t>(f.index);
        uint32_t& w = code_[f.index];
        switch (f.kind) {
        case FixupKind::B:
        case FixupKind::BL: {
            const int64_t words = diff;
            if (words < -(1 << 25) || words >= (1 << 25)) return false;
            const uint32_t imm26 = static_cast<uint32_t>(words) & 0x03FFFFFF;
            w |= imm26;
            break;
        }
        case FixupKind::CBZ:
        case FixupKind::CBNZ:
        case FixupKind::BCOND: {
            if (diff < -(1 << 18) || diff >= (1 << 18)) return false;
            const uint32_t imm19 = static_cast<uint32_t>(diff) & 0x07FFFF;
            w |= imm19 << 5;
            break;
        }
        }
    }
    fixups_.clear();
    return true;
}

void Assembler::movz(uint8_t rd, uint16_t imm16, int hw) {
    emit(0xD2800000u | (static_cast<uint32_t>(hw) << 21) | (static_cast<uint32_t>(imm16) << 5) | rd);
}
void Assembler::movk(uint8_t rd, uint16_t imm16, int hw) {
    emit(0xF2800000u | (static_cast<uint32_t>(hw) << 21) | (static_cast<uint32_t>(imm16) << 5) | rd);
}
void Assembler::mov_imm64(uint8_t rd, uint64_t v) {
    if ((v & 0xFFFFFFFFFFFF0000ULL) == 0) {
        movz(rd, static_cast<uint16_t>(v), 0);
        return;
    }
    movz(rd, static_cast<uint16_t>(v & 0xFFFF), 0);
    if (v >> 16) movk(rd, static_cast<uint16_t>((v >> 16) & 0xFFFF), 1);
    if (v >> 32) movk(rd, static_cast<uint16_t>((v >> 32) & 0xFFFF), 2);
    if (v >> 48) movk(rd, static_cast<uint16_t>((v >> 48) & 0xFFFF), 3);
}
void Assembler::mov_reg64(uint8_t rd, uint8_t rm) { emit(0xAA0003E0u | (rm << 16) | rd); }
void Assembler::add_imm64(uint8_t rd, uint8_t rn, uint32_t imm12) {
    emit(0x91000000u | (imm12 << 10) | (rn << 5) | rd);
}
void Assembler::sub_imm64(uint8_t rd, uint8_t rn, uint32_t imm12) {
    emit(0xD1000000u | (imm12 << 10) | (rn << 5) | rd);
}
void Assembler::add_reg64(uint8_t rd, uint8_t rn, uint8_t rm) {
    emit(0x8B000000u | (rm << 16) | (rn << 5) | rd);
}
void Assembler::sub_reg64(uint8_t rd, uint8_t rn, uint8_t rm) {
    emit(0xCB000000u | (rm << 16) | (rn << 5) | rd);
}
void Assembler::and_reg64(uint8_t rd, uint8_t rn, uint8_t rm) {
    emit(0x8A000000u | (rm << 16) | (rn << 5) | rd);
}
void Assembler::orr_reg64(uint8_t rd, uint8_t rn, uint8_t rm) {
    emit(0xAA000000u | (rm << 16) | (rn << 5) | rd);
}
void Assembler::eor_reg64(uint8_t rd, uint8_t rn, uint8_t rm) {
    emit(0xCA000000u | (rm << 16) | (rn << 5) | rd);
}
void Assembler::madd64(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t ra) {
    emit(0x9B000000u | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}
void Assembler::sdiv64(uint8_t rd, uint8_t rn, uint8_t rm) {
    emit(0x9AC00C00u | (rm << 16) | (rn << 5) | rd);
}
void Assembler::udiv64(uint8_t rd, uint8_t rn, uint8_t rm) {
    emit(0x9AC00800u | (rm << 16) | (rn << 5) | rd);
}
void Assembler::lslv64(uint8_t rd, uint8_t rn, uint8_t rm) {
    emit(0x9AC02000u | (rm << 16) | (rn << 5) | rd);
}
void Assembler::lsrv64(uint8_t rd, uint8_t rn, uint8_t rm) {
    emit(0x9AC02400u | (rm << 16) | (rn << 5) | rd);
}
void Assembler::asrv64(uint8_t rd, uint8_t rn, uint8_t rm) {
    emit(0x9AC02C00u | (rm << 16) | (rn << 5) | rd);
}

void Assembler::ldr64(uint8_t rt, uint8_t rn, uint32_t off) {
    emit(0xF9400000u | ((off >> 3) << 10) | (rn << 5) | rt);
}
void Assembler::str64(uint8_t rt, uint8_t rn, uint32_t off) {
    emit(0xF9000000u | ((off >> 3) << 10) | (rn << 5) | rt);
}
void Assembler::ldr32(uint8_t rt, uint8_t rn, uint32_t off) {
    emit(0xB9400000u | ((off >> 2) << 10) | (rn << 5) | rt);
}
void Assembler::str32(uint8_t rt, uint8_t rn, uint32_t off) {
    emit(0xB9000000u | ((off >> 2) << 10) | (rn << 5) | rt);
}
void Assembler::ldr16(uint8_t rt, uint8_t rn, uint32_t off) {
    emit(0x79400000u | ((off >> 1) << 10) | (rn << 5) | rt);
}
void Assembler::str16(uint8_t rt, uint8_t rn, uint32_t off) {
    emit(0x79000000u | ((off >> 1) << 10) | (rn << 5) | rt);
}
void Assembler::ldr8(uint8_t rt, uint8_t rn, uint32_t off) {
    emit(0x39400000u | (off << 10) | (rn << 5) | rt);
}
void Assembler::str8(uint8_t rt, uint8_t rn, uint32_t off) {
    emit(0x39000000u | (off << 10) | (rn << 5) | rt);
}
void Assembler::stp64(uint8_t rt, uint8_t rt2, uint8_t rn, int64_t off) {
    const int64_t imm7 = off / 8;
    emit(0xA9000000u | (static_cast<uint32_t>(imm7 & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt);
}
void Assembler::ldp64(uint8_t rt, uint8_t rt2, uint8_t rn, int64_t off) {
    const int64_t imm7 = off / 8;
    emit(0xA9400000u | (static_cast<uint32_t>(imm7 & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt);
}

void Assembler::cmp_imm64(uint8_t rn, uint32_t imm12) {
    emit(0xF1000000u | (imm12 << 10) | (rn << 5) | 31);
}
void Assembler::cmp_reg64(uint8_t rn, uint8_t rm) {
    emit(0xEB000000u | (rm << 16) | (rn << 5) | 31);
}
void Assembler::cset64(uint8_t rd, Cond c) {
    const uint32_t inv = static_cast<uint32_t>(c ^ 1);
    emit(0x9A800000u | (31u << 16) | (inv << 12) | (31u << 5) | rd);
}
void Assembler::b(int label) {
    fixups_.push_back(Fixup{FixupKind::B, code_.size(), label});
    emit(0x14000000u);
}
void Assembler::bl(int label) {
    fixups_.push_back(Fixup{FixupKind::BL, code_.size(), label});
    emit(0x94000000u);
}
void Assembler::bl_imm(size_t target_index) {
    const int64_t diff = static_cast<int64_t>(target_index) - static_cast<int64_t>(code_.size());
    const uint32_t imm26 = static_cast<uint32_t>(diff) & 0x03FFFFFF;
    emit(0x94000000u | imm26);
}
void Assembler::br(uint8_t rn) { emit(0xD61F0000u | (rn << 5)); }
void Assembler::blr(uint8_t rn) { emit(0xD63F0000u | (rn << 5)); }
void Assembler::ret(uint8_t rn) { emit(0xD65F0000u | (rn << 5)); }
void Assembler::cbz64(uint8_t rn, int label) {
    fixups_.push_back(Fixup{FixupKind::CBZ, code_.size(), label});
    emit(0xB4000000u | (rn << 5));
}
void Assembler::cbnz64(uint8_t rn, int label) {
    fixups_.push_back(Fixup{FixupKind::CBNZ, code_.size(), label});
    emit(0xB5000000u | (rn << 5));
}
void Assembler::b_cond(Cond c, int label) {
    fixups_.push_back(Fixup{FixupKind::BCOND, code_.size(), label});
    emit(0x54000000u | static_cast<uint32_t>(c));
}
void Assembler::adr(uint8_t rd, int64_t byte_off) {
    const uint32_t immlo = static_cast<uint32_t>(byte_off & 3) << 29;
    const uint32_t immhi = static_cast<uint32_t>((byte_off >> 2) & 0x7FFFF) << 5;
    emit(0x10000000u | immlo | immhi | rd);
}
void Assembler::nop() { emit(0xD503201Fu); }
void Assembler::brk() { emit(0xD4200000u); }
void Assembler::dmb_ish() { emit(0xD5033BBFu); }

} // namespace armsx5::cpu::arm64
