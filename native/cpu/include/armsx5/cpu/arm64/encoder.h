#pragma once
// ARM64 (AArch64) machine-code emitter for the ARMSX5 JIT.
//
// Every emit_* returns the encoded 32-bit word; encoding correctness is
// covered by unit tests that compare against known-good words from the
// ARM Architecture Reference Manual. No dependency on an external assembler.
#include <cstdint>
#include <vector>

namespace armsx5::cpu::arm64 {

enum Cond : uint8_t {
    EQ = 0, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL, NV = 15,
};

// Maps x86 condition codes (0=O .. 15=G) to AArch64 conditions.
Cond cond_from_x86(uint8_t x86_cond);

class Assembler {
public:
    int new_label();
    void bind(int label);
    // Resolves all pending branch fixups. Returns false if any label was
    // never bound or a branch is out of range.
    bool finalize();

    const std::vector<uint32_t>& code() const { return code_; }
    size_t pc() const { return code_.size(); }
    void emit(uint32_t word) { code_.push_back(word); }

    // ---- data movement / arithmetic ----
    void movz(uint8_t rd, uint16_t imm16, int shift_words); // shift_words: 0..3 (x16 bits)
    void movk(uint8_t rd, uint16_t imm16, int shift_words);
    void mov_imm64(uint8_t rd, uint64_t value);
    void mov_reg64(uint8_t rd, uint8_t rm); // ORR rd, XZR, rm
    void add_imm64(uint8_t rd, uint8_t rn, uint32_t imm12);
    void sub_imm64(uint8_t rd, uint8_t rn, uint32_t imm12);
    void add_reg64(uint8_t rd, uint8_t rn, uint8_t rm);
    void sub_reg64(uint8_t rd, uint8_t rn, uint8_t rm);
    void and_reg64(uint8_t rd, uint8_t rn, uint8_t rm);
    void orr_reg64(uint8_t rd, uint8_t rn, uint8_t rm);
    void eor_reg64(uint8_t rd, uint8_t rn, uint8_t rm);
    void madd64(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t ra);
    void sdiv64(uint8_t rd, uint8_t rn, uint8_t rm);
    void udiv64(uint8_t rd, uint8_t rn, uint8_t rm);
    void lslv64(uint8_t rd, uint8_t rn, uint8_t rm);
    void lsrv64(uint8_t rd, uint8_t rn, uint8_t rm);
    void asrv64(uint8_t rd, uint8_t rn, uint8_t rm);

    // ---- loads / stores (unsigned imm12, scaled by access size) ----
    void ldr64(uint8_t rt, uint8_t rn, uint32_t byte_off);
    void str64(uint8_t rt, uint8_t rn, uint32_t byte_off);
    void ldr32(uint8_t rt, uint8_t rn, uint32_t byte_off);
    void str32(uint8_t rt, uint8_t rn, uint32_t byte_off);
    void ldr16(uint8_t rt, uint8_t rn, uint32_t byte_off);
    void str16(uint8_t rt, uint8_t rn, uint32_t byte_off);
    void ldr8(uint8_t rt, uint8_t rn, uint32_t byte_off);
    void str8(uint8_t rt, uint8_t rn, uint32_t byte_off);
    void stp64(uint8_t rt, uint8_t rt2, uint8_t rn, int64_t byte_off);
    void ldp64(uint8_t rt, uint8_t rt2, uint8_t rn, int64_t byte_off);

    // ---- control flow ----
    void cmp_imm64(uint8_t rn, uint32_t imm12);
    void cmp_reg64(uint8_t rn, uint8_t rm);
    void cset64(uint8_t rd, Cond c);
    void b(int label);
    void bl(int label);
    void bl_imm(size_t target_index); // direct call to a code index
    void br(uint8_t rn);
    void blr(uint8_t rn);
    void ret(uint8_t rn = 30);
    void cbz64(uint8_t rn, int label);
    void cbnz64(uint8_t rn, int label);
    void b_cond(Cond c, int label);
    void adr(uint8_t rd, int64_t byte_off_from_pc);
    void nop();
    void brk();
    void dmb_ish();

private:
    enum class FixupKind : uint8_t { B, BL, CBZ, CBNZ, BCOND };
    struct Fixup {
        FixupKind kind;
        size_t index;
        int label;
    };
    std::vector<uint32_t> code_;
    std::vector<Fixup> fixups_;
    std::vector<size_t> label_pos_; // 0 = unbound (index 0 reserved as unbound)
};

} // namespace armsx5::cpu::arm64
