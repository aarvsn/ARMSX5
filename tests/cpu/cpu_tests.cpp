// CPU engine tests: decoder, interpreter semantics (integer + SSE), flags,
// ARM64 encoder encodings, and JIT block translation/caching (host-safe).
#include <gtest/gtest.h>

#include "armsx5/cpu/arm64/encoder.h"
#include "armsx5/cpu/cpu_state.h"
#include "armsx5/cpu/interpreter.h"
#include "armsx5/cpu/jit.h"
#include "armsx5/cpu/x86/decoder.h"
#include "armsx5/memory/guest_memory.h"
#include "armsx5/system/kernel.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace armsx5;

namespace {

// ModRM byte builder for tests: mod, reg, rm.
constexpr uint8_t modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
    return static_cast<uint8_t>((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

struct RunOut {
    cpu::CpuState state;
    cpu::RunStats stats;
    std::string stdout_text;
};

void run_guest(RunOut& out, const std::vector<uint8_t>& code, uint64_t base = 0x400000,
               uint64_t max_steps = 100000) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(base, 0x10000, memory::Prot::R | memory::Prot::W | memory::Prot::X)
                    .has_value());
    ASSERT_TRUE(mem.write(base, code.data(), code.size()).has_value());
    mem.mark_exec(base, 0x10000);

    system::CaptureSink sink;
    system::Kernel kernel(mem);
    kernel.set_output_sink(&sink);

    cpu::Interpreter interp(mem);
    interp.set_syscall_handler(&kernel);
    interp.state().rip = base;
    interp.state().gpr[cpu::reg::SP] = base + 0x8000;

    out.stats = interp.run(max_steps);
    out.state = interp.state();
    out.stdout_text = sink.captured();
}

} // namespace

// ---------- decoder ----------
TEST(Decoder, DecodesMovImm64) {
    // mov rax, 0x1122334455667788  (48 B8 imm64)
    const uint8_t code[] = {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    auto d = cpu::x86::decode(code, sizeof(code));
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->cls, cpu::x86::InstrClass::MovImm);
    EXPECT_EQ(d->length, 10);
    EXPECT_EQ(d->opsize, 8);
    EXPECT_EQ(d->src.imm, static_cast<int64_t>(0x1122334455667788ULL));
}

TEST(Decoder, DecodesAddRegReg) {
    // add rax, rbx (48 01 D8)
    const uint8_t code[] = {0x48, 0x01, modrm(3, 3, 0)}; // modrm D8: reg=rbx, rm=rax
    auto d = cpu::x86::decode(code, sizeof(code));
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->cls, cpu::x86::InstrClass::Alu);
    EXPECT_EQ(d->alu_op, cpu::x86::AluOp::Add);
    EXPECT_EQ(d->dst.reg, cpu::reg::AX);
    EXPECT_EQ(d->src.reg, cpu::reg::BX);
    EXPECT_EQ(d->length, 3);
}

TEST(Decoder, DecodesLeaRipRelative) {
    // lea rax, [rip+0x10] (48 8D 05 10 00 00 00)
    const uint8_t code[] = {0x48, 0x8D, modrm(0, 0, 5), 0x10, 0x00, 0x00, 0x00};
    auto d = cpu::x86::decode(code, sizeof(code));
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->cls, cpu::x86::InstrClass::Lea);
    EXPECT_TRUE(d->src.mem.rip_relative);
    EXPECT_EQ(d->src.mem.disp, 0x10);
}

TEST(Decoder, RejectsUnknownOpcodeAsUnsupported) {
    // 0F 0F is not in the implemented two-byte set.
    const uint8_t code[] = {0x0F, 0x0F, 0x00};
    auto d = cpu::x86::decode(code, sizeof(code));
    ASSERT_FALSE(d.has_value());
    EXPECT_EQ(d.error().code, Status::Unsupported);
}

TEST(Decoder, RejectsTruncatedInstruction) {
    const uint8_t code[] = {0x48, 0xB8, 0x88}; // mov imm64 cut short
    auto d = cpu::x86::decode(code, sizeof(code));
    ASSERT_FALSE(d.has_value());
    EXPECT_EQ(d.error().code, Status::ParseError);
}

TEST(Decoder, DecodesSyscall) {
    const uint8_t code[] = {0x0F, 0x05};
    auto d = cpu::x86::decode(code, sizeof(code));
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->cls, cpu::x86::InstrClass::Syscall);
}

// ---------- interpreter semantics ----------
TEST(Interpreter, ArithmeticAndFlagsAdd) {
    // mov rax, 0x7FFF...F; add rax, 1 -> overflow (OF), SF set.
    std::vector<uint8_t> code = {0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F,
                                 0x48, 0x83, 0xC0, 0x01};
    RunOut out;
    run_guest(out, code, 0x400000, 2);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::MaxSteps);
    EXPECT_EQ(out.stats.steps, 2u);
    EXPECT_EQ(out.state.gpr[cpu::reg::AX], 0x8000000000000000ULL);
    EXPECT_TRUE(out.state.flags.of);
    EXPECT_TRUE(out.state.flags.sf);
    EXPECT_FALSE(out.state.flags.zf);
    EXPECT_FALSE(out.state.flags.cf);
}

TEST(Interpreter, SubBorrowSetsCF) {
    std::vector<uint8_t> code = {0x48, 0x31, 0xC0,        // xor rax, rax
                                 0x48, 0x83, 0xE8, 0x01}; // sub rax, 1
    RunOut out;
    run_guest(out, code, 0x400000, 2);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::MaxSteps);
    EXPECT_EQ(out.state.gpr[cpu::reg::AX], ~0ULL);
    EXPECT_TRUE(out.state.flags.cf);
    EXPECT_FALSE(out.state.flags.zf);
    EXPECT_TRUE(out.state.flags.sf);
}

TEST(Interpreter, ZeroFlagFromXor) {
    std::vector<uint8_t> code = {0x48, 0xB8, 0x11, 0x11, 0, 0, 0, 0, 0, 0, // mov rax, 0x1111
                                 0x48, 0x31, 0xC0};                        // xor rax, rax
    RunOut out;
    run_guest(out, code, 0x400000, 2);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::MaxSteps);
    EXPECT_TRUE(out.state.flags.zf);
    EXPECT_FALSE(out.state.flags.of);
    EXPECT_FALSE(out.state.flags.cf);
}

TEST(Interpreter, LoopSumsWithJbe) {
    std::vector<uint8_t> code = {
        0x48, 0x31, 0xC0,                   // xor rax, rax        (sum)
        0x48, 0xC7, 0xC1, 0x01, 0, 0, 0,    // mov rcx, 1          (i)
        0x48, 0x01, 0xC8,                   // L: add rax, rcx
        0x48, 0xFF, 0xC1,                   // inc rcx
        0x48, 0x83, 0xF9, 0x64,             // cmp rcx, 100
        0x76, 0xF4,                         // jbe L (rel8 = -12: 10 - 22)
        0x48, 0x89, 0xC7,                   // mov rdi, rax
        0xB8, 0x01, 0, 0, 0,                // mov eax, 1
        0x0F, 0x05,
    };
    RunOut out;
    run_guest(out, code);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Exited);
    EXPECT_EQ(out.stats.exit_code, 5050u);
}

TEST(Interpreter, CallAndRetRoundTrip) {
    std::vector<uint8_t> code = {
        0xBF, 0x05, 0, 0, 0,      // mov edi, 5 (BF + imm32)
        0xE8, 0x0A, 0, 0, 0,      // call +10 -> add7 (rip_after=10, target=20)
        0x48, 0x89, 0xC7,         // mov rdi, rax
        0xB8, 0x01, 0, 0, 0,      // mov eax, 1
        0x0F, 0x05,               // syscall
        // add7:
        0x48, 0x8D, 0x47, 0x07,   // lea rax, [rdi+7]
        0xC3,                     // ret
    };
    RunOut out;
    run_guest(out, code);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Exited);
    EXPECT_EQ(out.stats.exit_code, 12u);
}

TEST(Interpreter, WriteSyscallCapturesOutput) {
    std::vector<uint8_t> code = {
        0x48, 0xB8, 0x00, 0x01, 0x40, 0, 0, 0, 0, 0, // mov rax, 0x400100 (msg at base+0x100)
        0x48, 0x89, 0xC6,                            // mov rsi, rax
        0xBA, 0x18, 0, 0, 0,                         // mov edx, 24
        0xBF, 0x01, 0, 0, 0,                         // mov edi, 1
        0xB8, 0x04, 0, 0, 0,                         // mov eax, 4 (write)
        0x0F, 0x05,
        0x31, 0xFF,                                  // xor edi, edi
        0xB8, 0x01, 0, 0, 0,                         // mov eax, 1
        0x0F, 0x05,
    };
    // Message lives at base+0x100; run_guest maps [0x400000, 0x410000).
    while (code.size() < 0x100) code.push_back(0x90); // nop padding
    const char msg[] = "Hello from ARMSX5 guest\n";
    for (size_t i = 0; i < sizeof(msg); ++i) code.push_back(static_cast<uint8_t>(msg[i]));
    RunOut out;
    run_guest(out, code);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Exited);
    EXPECT_EQ(out.stats.exit_code, 0u);
    EXPECT_EQ(out.stdout_text, "Hello from ARMSX5 guest\n");
}

TEST(Interpreter, MemoryLoadStore) {
    std::vector<uint8_t> code = {
        0x48, 0xB8, 0xEE, 0xFF, 0xC0, 0, 0, 0, 0, 0, // mov rax, 0xC0FFEE
        0x48, 0x89, 0x44, 0x24, 0xF8,                // mov [rsp-8], rax
        0x48, 0x8B, 0x44, 0x24, 0xF8,                // mov rax, [rsp-8]
        0x48, 0x89, 0xC7,                            // mov rdi, rax
        0xB8, 0x01, 0, 0, 0,
        0x0F, 0x05,
    };
    RunOut out;
    run_guest(out, code);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Exited);
    EXPECT_EQ(out.stats.exit_code, 0xC0FFEEu);
}

TEST(Interpreter, UnsupportedInstructionStopsWithoutSilence) {
    // pxor would be supported; use AESIMC (66 0F 38 DB) -> unsupported.
    std::vector<uint8_t> code = {0x66, 0x0F, 0x38, 0xDB, 0xC0};
    RunOut out;
    run_guest(out, code);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Unsupported);
    EXPECT_EQ(out.stats.status, Status::Unsupported);
}

TEST(Interpreter, DivideByZeroIsGuestFault) {
    std::vector<uint8_t> code = {
        0x31, 0xC9,             // xor ecx, ecx
        0xF7, 0xF1,             // div ecx
    };
    RunOut out;
    run_guest(out, code);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Fault);
    EXPECT_NE(out.stats.message.find("zero"), std::string::npos);
}

TEST(Interpreter, PushPopPreservesUpperBitsOn32Bit) {
    std::vector<uint8_t> code = {
        0x48, 0xC7, 0xC4, 0x00, 0x80, 0x40, 0,  // mov rsp, 0x408000
        0xB8, 0x78, 0x56, 0x34, 0x12,           // mov eax, 0x12345678 (zero-extends rax)
        0x50,                                   // push rax
        0x5B,                                   // pop rbx
        0x48, 0x89, 0xDF,                       // mov rdi, rbx
        0xB8, 0x01, 0, 0, 0,
        0x0F, 0x05,
    };
    RunOut out;
    run_guest(out, code);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Exited);
    EXPECT_EQ(out.stats.exit_code, 0x12345678u);
    EXPECT_EQ(out.state.gpr[cpu::reg::BX], 0x12345678u);
}

TEST(Interpreter, ShiftSemantics) {
    std::vector<uint8_t> code = {
        0xB8, 0x01, 0, 0, 0,          // mov eax, 1
        0xC1, 0xE0, 0x1F,             // shl eax, 31
    };
    RunOut out;
    run_guest(out, code, 0x400000, 2);
    EXPECT_EQ(out.state.gpr[cpu::reg::AX], 0x80000000u);
    // CF = last bit shifted out = bit 31 of the original value (1) = 0.
    EXPECT_FALSE(out.state.flags.cf);
    // OF is only defined for count==1; count here is 31 so OF stays false.
    EXPECT_FALSE(out.state.flags.of);
}

// ---------- register access semantics ----------
TEST(CpuState, ZeroExtendOn32BitWrite) {
    cpu::CpuState s;
    s.gpr[cpu::reg::AX] = 0xDEADBEEF12345678ULL;
    cpu::write_reg(s, cpu::reg::AX, 4, 0x11111111);
    EXPECT_EQ(s.gpr[cpu::reg::AX], 0x0000000011111111ULL);
}

TEST(CpuState, PartialWritesPreserveBits) {
    cpu::CpuState s;
    s.gpr[cpu::reg::AX] = 0xAABBCCDD11223344ULL;
    cpu::write_reg(s, cpu::reg::AX, 2, 0x9876);
    EXPECT_EQ(s.gpr[cpu::reg::AX], 0xAABBCCDD11229876ULL); // low 16 bits replaced
    cpu::write_reg(s, cpu::reg::AX, 1, 0xAB, true);        // AH = bits 8..15
    EXPECT_EQ(s.gpr[cpu::reg::AX], 0xAABBCCDD1122AB76ULL);
}

// ---------- SSE subset ----------
TEST(Sse, MovssLoadPreservesUpperLanes) {
    // movss xmm1, xmm0 (F3 0F 10 modrm(mod3, dst=1, src=0))
    std::vector<uint8_t> code = {
        0xB8, 0x7B, 0x00, 0x00, 0x40,             // mov eax, 0x40007B (1.5f bits) — via gpr
        0x66, 0x0F, 0x6E, modrm(3, 0, 0),         // movd xmm0, eax
        0x48, 0xB9, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, // movabs rcx, 0x1111111111111111
        0x66, 0x48, 0x0F, 0x6E, modrm(3, 1, 1),   // movq xmm1, rcx
        0xF3, 0x0F, 0x10, modrm(3, 1, 0),         // movss xmm1, xmm0
    };
    RunOut out;
    run_guest(out, code, 0x400000, 5); // exactly the 5 encoded instructions
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::MaxSteps);
    const uint64_t expect_lo = 0x111111114000007BULL; // lane0 replaced, upper of lo preserved
    EXPECT_EQ(out.state.xmm[1].lo, expect_lo);
}

TEST(Sse, AddssAndAddps) {
    // movd xmm0, [mem 1.5f]; addss xmm2, xmm0 ... use reg-only paths:
    // movd xmm0, eax(1.5f); movd xmm1, eax(2.5f via 0x40200000); addss xmm0, xmm1
    std::vector<uint8_t> code = {
        0xB8, 0x00, 0x00, 0xC0, 0x3F,             // mov eax, 1.5f
        0x66, 0x0F, 0x6E, modrm(3, 0, 0),         // movd xmm0, eax
        0xB8, 0x00, 0x00, 0x20, 0x40,             // mov eax, 2.5f
        0x66, 0x0F, 0x6E, modrm(3, 1, 0),         // movd xmm1, eax
        0xF3, 0x0F, 0x58, modrm(3, 0, 1),         // addss xmm0, xmm1
    };
    RunOut out;
    run_guest(out, code, 0x400000, 7);
    const uint32_t bits = static_cast<uint32_t>(out.state.xmm[0].lo & 0xFFFFFFFF);
    float f;
    std::memcpy(&f, &bits, 4);
    EXPECT_FLOAT_EQ(f, 4.0f);
}

TEST(Sse, PackedAddpsFourLanes) {
    // Build xmm0 = {1,2,3,4}, xmm1 = {10,20,30,40}; addps xmm0, xmm1
    // via: mov eax,imm; movd; pshufd to broadcast; addps.
    // Simpler: use memory-based movaps loads from a table at base+0x100.
    std::vector<uint8_t> code = {
        0x0F, 0x28, 0x05, 0xF9, 0x00, 0x00, 0x00, // movaps xmm0, [rip+0xF9] -> base+0x100
        0x0F, 0x28, 0x0D, 0x02, 0x01, 0x00, 0x00, // movaps xmm1, [rip+0x102] -> base+0x110
        0x0F, 0x58, modrm(3, 0, 1),               // addps xmm0, xmm1
        0x66, 0x0F, 0x7E, modrm(3, 0, 0),         // movd eax, xmm0  (lane 0 = 11.0f)
        0xBF, 0x00, 0x00, 0x00, 0x00,             // mov edi, 0
        0xB8, 0x01, 0, 0, 0,                      // mov eax, 1
        0x0F, 0x05,                               // exit(0)
    };
    while (code.size() < 0x100) code.push_back(0x90);
    const float a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    const float b[4] = {10.0f, 20.0f, 30.0f, 40.0f};
    for (float v : a) { // table A at base+0x100 (16 bytes: 4 float lanes)
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        for (int k = 0; k < 4; ++k) code.push_back((bits >> (8 * k)) & 0xFF);
    }
    while (code.size() < 0x110) code.push_back(0x90);
    for (float v : b) { // table B at base+0x110 (16 bytes)
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        for (int k = 0; k < 4; ++k) code.push_back((bits >> (8 * k)) & 0xFF);
    }
    RunOut out;
    run_guest(out, code);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Exited);
    const uint32_t bits = static_cast<uint32_t>(out.state.xmm[0].lo & 0xFFFFFFFF);
    float f;
    std::memcpy(&f, &bits, 4);
    EXPECT_FLOAT_EQ(f, 11.0f);
    // lane 2 (in hi): 33.0f
    const uint32_t bits2 = static_cast<uint32_t>(out.state.xmm[0].hi & 0xFFFFFFFF);
    float f2;
    std::memcpy(&f2, &bits2, 4);
    EXPECT_FLOAT_EQ(f2, 33.0f);
}

TEST(Sse, ComissSetsFlags) {
    // movd xmm0, 1.0f; movd xmm1, 2.0f; comiss xmm0, xmm1 -> CF=1 (a<b)
    std::vector<uint8_t> code = {
        0xB8, 0x00, 0x00, 0x80, 0x3F,             // mov eax, 1.0f
        0x66, 0x0F, 0x6E, modrm(3, 0, 0),         // movd xmm0, eax
        0xB8, 0x00, 0x00, 0x00, 0x40,             // mov eax, 2.0f
        0x66, 0x0F, 0x6E, modrm(3, 1, 0),         // movd xmm1, eax
        0x0F, 0x2F, modrm(3, 0, 1),               // comiss xmm0, xmm1
    };
    RunOut out;
    run_guest(out, code, 0x400000, 5);
    EXPECT_TRUE(out.state.flags.cf);
    EXPECT_FALSE(out.state.flags.zf);
    EXPECT_FALSE(out.state.flags.of);
}

TEST(Sse, CmpssEqPredicate) {
    // movd xmm0, 1.0f; movd xmm1, 1.0f; cmpss xmm0, xmm1, 0 (EQ)
    std::vector<uint8_t> code = {
        0xB8, 0x00, 0x00, 0x80, 0x3F,
        0x66, 0x0F, 0x6E, modrm(3, 0, 0),
        0x66, 0x0F, 0x6E, modrm(3, 1, 0),
        0xF3, 0x0F, 0xC2, modrm(3, 0, 1), 0x00,  // cmpss xmm0, xmm1, EQ
    };
    RunOut out;
    run_guest(out, code, 0x400000, 5);
    EXPECT_EQ(out.state.xmm[0].lo & 0xFFFFFFFF, 0xFFFFFFFFu); // all-ones on true
}

TEST(Sse, Sqrtss) {
    // movd xmm0, 9.0f; sqrtss xmm1, xmm0 -> 3.0f
    std::vector<uint8_t> code = {
        0xB8, 0x00, 0x00, 0x10, 0x41,             // mov eax, 9.0f
        0x66, 0x0F, 0x6E, modrm(3, 0, 0),         // movd xmm0, eax
        0xF3, 0x0F, 0x51, modrm(3, 1, 0),         // sqrtss xmm1, xmm0
        0xF3, 0x0F, 0x10, modrm(3, 2, 1),         // movss xmm2, xmm1
        0xF3, 0x0F, 0x11, modrm(3, 2, 2),         // movss xmm2, xmm2 (nop-shaped)
    };
    RunOut out;
    run_guest(out, code, 0x400000, 4);
    const uint32_t bits = static_cast<uint32_t>(out.state.xmm[1].lo & 0xFFFFFFFF);
    float f;
    std::memcpy(&f, &bits, 4);
    EXPECT_FLOAT_EQ(f, 3.0f);
}

TEST(Sse, Cvtsi2ssAndCvttss2si) {
    // mov eax, 10; cvtsi2ss xmm0, eax; cvttss2si ebx, xmm0 -> 10
    std::vector<uint8_t> code = {
        0xB8, 0x0A, 0x00, 0x00, 0x00,             // mov eax, 10
        0xF3, 0x0F, 0x2A, modrm(3, 0, 0),         // cvtsi2ss xmm0, eax
        0xF3, 0x0F, 0x2C, modrm(3, 3, 0),         // cvttss2si ebx, xmm0
        0x89, 0xDF,                               // mov edi, ebx
        0xB8, 0x01, 0, 0, 0,                      // mov eax, 1
        0x0F, 0x05,                               // exit(10)
    };
    RunOut out;
    run_guest(out, code);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Exited);
    EXPECT_EQ(out.stats.exit_code, 10u);
}

TEST(Sse, MovdqaUnalignedFaults) {
    // movaps xmm0, [rsp+1] (unaligned) -> guest fault
    std::vector<uint8_t> code = {
        0x0F, 0x28, 0x44, 0x24, 0x01,             // movaps xmm0, [rsp+1]
    };
    RunOut out;
    run_guest(out, code, 0x400000, 1);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Fault);
    EXPECT_NE(out.stats.message.find("alignment"), std::string::npos);
}

TEST(Sse, PaddqPsubd) {
    // paddq xmm0, xmm1 with known halves; psubd check
    std::vector<uint8_t> code = {
        // mov rax, 1; movq xmm0, rax  (66 48 0F 6E C0)
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1
        0x66, 0x48, 0x0F, 0x6E, modrm(3, 0, 0),   // movq xmm0, rax
        0x48, 0xC7, 0xC1, 0x02, 0x00, 0x00, 0x00, // mov rcx, 2
        0x66, 0x48, 0x0F, 0x6E, modrm(3, 1, 1),   // movq xmm1, rcx
        0x66, 0x0F, 0xD4, modrm(3, 0, 1),         // paddq xmm0, xmm1
        0x66, 0x48, 0x0F, 0x7E, modrm(3, 0, 6),   // movq rsi, xmm0 (reg=xmm0 src, rm=rsi)
        0x48, 0x89, 0xF7,                         // mov rdi, rsi
        0xB8, 0x01, 0, 0, 0,                      // mov eax, 1
        0x0F, 0x05,                               // exit(3)
    };
    RunOut out;
    run_guest(out, code);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Exited);
    EXPECT_EQ(out.stats.exit_code, 3u);
}

TEST(Sse, PshufdShufflesLanes) {
    // xmm0 = {1,2,3,4} via memory; pshufd xmm1, xmm0, 0b00011011 (reverse)
    // then read lane 0 (= old lane 3 = 4) via movd.
    std::vector<uint8_t> code = {
        0x0F, 0x28, 0x05, 0xF9, 0x00, 0x00, 0x00, // movaps xmm0, [rip+0xF9] -> base+0x100
        0x66, 0x0F, 0x70, modrm(3, 1, 0), 0x1B,   // pshufd xmm1, xmm0, 0x1B (reverse)
        0xB8, 0x01, 0, 0, 0,                      // mov eax, 1 (exit code unused)
        0x0F, 0x05,                               // exit
    };
    while (code.size() < 0x100) code.push_back(0x90);
    const float a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    for (float v : a) { // 4 consecutive float lanes in 16 bytes
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        for (int k = 0; k < 4; ++k) code.push_back((bits >> (8 * k)) & 0xFF);
    }
    RunOut out;
    run_guest(out, code);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Exited);
    // shuffle imm 0x1B: lane 0 of xmm1 = old lane 3 = 4.0f
    const uint32_t bits = static_cast<uint32_t>(out.state.xmm[1].lo & 0xFFFFFFFF);
    float f;
    std::memcpy(&f, &bits, 4);
    EXPECT_FLOAT_EQ(f, 4.0f);
}

TEST(Sse, AvxVexEncodingRejected) {
    // VEX-encoded vpxor (C5 DB EF C0-ish): must be Unsupported, not executed.
    std::vector<uint8_t> code = {0xC5, 0xDB, 0xEF, 0xC0};
    RunOut out;
    run_guest(out, code, 0x400000, 1);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Unsupported);
}

// ---------- ARM64 encoder (encoding-correctness battery) ----------
TEST(Arm64Encoder, KnownEncodings) {
    using armsx5::cpu::arm64::Assembler;
    Assembler a;
    a.movz(0, 1, 0);      // mov x0, #1
    a.add_imm64(0, 1, 2); // add x0, x1, #2
    a.str64(0, 31, 0);    // str x0, [sp]
    a.ret(30);            // ret
    a.ldr64(2, 3, 16);    // ldr x2, [x3, #16]
    a.add_reg64(4, 5, 6); // add x4, x5, x6
    ASSERT_TRUE(a.finalize());
    const auto& c = a.code();
    EXPECT_EQ(c[0], 0xD2800020u);
    EXPECT_EQ(c[1], 0x91000820u);
    EXPECT_EQ(c[2], 0xF90003E0u);
    EXPECT_EQ(c[3], 0xD65F03C0u);
    EXPECT_EQ(c[4], 0xF9400862u);
    EXPECT_EQ(c[5], 0x8B0600A4u);
}

TEST(Arm64Encoder, MovImm64UsesMovzMovk) {
    using armsx5::cpu::arm64::Assembler;
    Assembler a;
    a.mov_imm64(9, 0x1234567890ABCDEFULL);
    ASSERT_TRUE(a.finalize());
    ASSERT_EQ(a.code().size(), 4u);
    EXPECT_EQ(a.code()[0], 0xD299BDE9u); // movz x9, #0xCDEF
    EXPECT_EQ(a.code()[1], 0xF2B21569u); // movk x9, #0x90AB, lsl #16
    EXPECT_EQ(a.code()[2], 0xF2CACF09u); // movk x9, #0x5678, lsl #32
    EXPECT_EQ(a.code()[3], 0xF2E24689u); // movk x9, #0x1234, lsl #48
}

TEST(Arm64Encoder, BranchFixupsResolve) {
    using armsx5::cpu::arm64::Assembler;
    Assembler a;
    const int done = a.new_label();
    a.cbnz64(0, done);
    a.nop();
    a.bind(done);
    a.ret(30);
    ASSERT_TRUE(a.finalize());
    // CBNZ at index 0 must point to index 2: diff = +2 -> imm19 = 2 << 5.
    EXPECT_EQ(a.code()[0], 0xB5000040u);
    EXPECT_EQ(a.code()[1], 0xD503201Fu);
    EXPECT_EQ(a.code()[2], 0xD65F03C0u);
}

TEST(Arm64Encoder, UnboundLabelFailsFinalize) {
    using armsx5::cpu::arm64::Assembler;
    Assembler a;
    const int never = a.new_label();
    a.b(never);
    EXPECT_FALSE(a.finalize());
}

TEST(Arm64Encoder, X86ConditionMapping) {
    namespace a64 = armsx5::cpu::arm64;
    EXPECT_EQ(a64::cond_from_x86(4), a64::EQ);   // E -> EQ
    EXPECT_EQ(a64::cond_from_x86(5), a64::NE);   // NE -> NE
    EXPECT_EQ(a64::cond_from_x86(2), a64::CS);   // B -> CS
    EXPECT_EQ(a64::cond_from_x86(0), a64::VS);   // O -> VS
    EXPECT_EQ(a64::cond_from_x86(15), a64::GT);  // G -> GT
}

// ---------- JIT (host-safe: translation + cache only) ----------
TEST(Jit, TranslatesBlockAndCaches) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(0x400000, 0x1000,
                        memory::Prot::R | memory::Prot::W | memory::Prot::X).has_value());
    const uint8_t code[] = {
        0xB8, 0x01, 0, 0, 0,   // mov eax, 1
        0x48, 0x01, 0xC8,      // add rax, rcx
        0xC3,                  // ret
    };
    ASSERT_TRUE(mem.write(0x400000, code, sizeof(code)).has_value());
    mem.mark_exec(0x400000, 0x1000);

    cpu::Jit jit(mem);
    ASSERT_TRUE(jit.init().has_value());
    auto b1 = jit.get_block(0x400000);
    ASSERT_TRUE(b1.has_value());
    EXPECT_EQ((*b1)->guest_start, 0x400000u);
    EXPECT_EQ((*b1)->guest_end, 0x400000u + sizeof(code));
    EXPECT_GT((*b1)->code_words, 0u);
    auto b2 = jit.get_block(0x400000);
    ASSERT_TRUE(b2.has_value());
    EXPECT_EQ(b2.value(), b1.value()); // same cached pointer
    EXPECT_EQ(jit.stats().translations, 1u);
}

TEST(Jit, RejectsBlockInNonExecPage) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(0x410000, 0x1000, memory::Prot::R).has_value());
    cpu::Jit jit(mem);
    ASSERT_TRUE(jit.init().has_value());
    auto b = jit.get_block(0x410000);
    ASSERT_FALSE(b.has_value());
    EXPECT_EQ(b.error().code, Status::GuestFault);
}

TEST(Jit, UnsupportedInstructionEndsBlockBeforeIt) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(0x420000, 0x1000,
                        memory::Prot::R | memory::Prot::W | memory::Prot::X).has_value());
    // mov eax,1 then an instruction the JIT cannot translate.
    const uint8_t code[] = {0xB8, 0x01, 0, 0, 0, 0x66, 0x0F, 0x38, 0xDB, 0xC0};
    ASSERT_TRUE(mem.write(0x420000, code, sizeof(code)).has_value());
    mem.mark_exec(0x420000, 0x1000);
    cpu::Jit jit(mem);
    ASSERT_TRUE(jit.init().has_value());
    auto b = jit.get_block(0x420000);
    ASSERT_TRUE(b.has_value());
    // Block ends before the unsupported instruction (5 bytes in).
    EXPECT_EQ((*b)->guest_end, 0x420005u);
}

TEST(Jit, InvalidateAllClearsCache) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(0x430000, 0x1000,
                        memory::Prot::R | memory::Prot::W | memory::Prot::X).has_value());
    const uint8_t code[] = {0xC3};
    ASSERT_TRUE(mem.write(0x430000, code, sizeof(code)).has_value());
    mem.mark_exec(0x430000, 0x1000);
    cpu::Jit jit(mem);
    ASSERT_TRUE(jit.init().has_value());
    ASSERT_TRUE(jit.get_block(0x430000).has_value());
    jit.invalidate_all();
    EXPECT_EQ(jit.stats().invalidations, 1u);
    auto b = jit.get_block(0x430000);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(jit.stats().translations, 2u); // retranslated after flush
}

TEST(Jit, NativeExecutionRequiresAarch64) {
    // On x86-64 hosts the JIT must refuse to execute (interpreter fallback).
    if (cpu::Jit::can_execute_natively()) {
        GTEST_SKIP() << "running on aarch64 host; native execution expected";
    }
    EXPECT_TRUE(cpu::Jit::can_execute_natively() == false);
}

// ---------- AVX (VEX) decoder ----------
namespace {
// Emits a RIP-relative vector instruction: bytes up to and including the
// ModRM byte, then the disp32 to target_off (relative to the code base).
void emit_rip_rel(std::vector<uint8_t>& code, std::initializer_list<uint8_t> head,
                  uint32_t target_off) {
    for (uint8_t b : head) code.push_back(b);
    const int64_t disp =
        static_cast<int64_t>(target_off) - static_cast<int64_t>(code.size() + 4);
    for (int k = 0; k < 4; ++k) code.push_back((disp >> (8 * k)) & 0xFF);
}

void append_f32(std::vector<uint8_t>& code, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    for (int k = 0; k < 4; ++k) code.push_back((bits >> (8 * k)) & 0xFF);
}
void append_i32(std::vector<uint8_t>& code, int32_t v) {
    for (int k = 0; k < 4; ++k) code.push_back((static_cast<uint32_t>(v) >> (8 * k)) & 0xFF);
}
float f32_at(const cpu::CpuState& s, int reg, int lane) {
    uint32_t b = lane < 2 ? static_cast<uint32_t>(s.xmm[reg].lo >> (32 * lane))
                          : static_cast<uint32_t>(s.xmm[reg].hi >> (32 * (lane - 2)));
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
float y32_at(const cpu::CpuState& s, int reg, int lane) {
    uint32_t b;
    if (lane < 2) b = static_cast<uint32_t>(s.xmm[reg].lo >> (32 * lane));
    else if (lane < 4) b = static_cast<uint32_t>(s.xmm[reg].hi >> (32 * (lane - 2)));
    else if (lane < 6) b = static_cast<uint32_t>(s.ymm_hi[reg].lo >> (32 * (lane - 4)));
    else b = static_cast<uint32_t>(s.ymm_hi[reg].hi >> (32 * (lane - 6)));
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
} // namespace

TEST(Avx, TwoByteVexDecode) {
    // C5 F2 58 C2 = vaddss xmm0, xmm1, xmm2 (VEX.NDS.LIG.F3.0F.WIG 58 /r)
    const uint8_t code[] = {0xC5, 0xF2, 0x58, 0xC2};
    auto d = cpu::x86::decode(code, sizeof(code));
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->cls, cpu::x86::InstrClass::SseArith);
    EXPECT_EQ(d->sse_op, cpu::x86::SseOp::Add);
    EXPECT_EQ(d->sse_form, cpu::x86::SseForm::SS);
    EXPECT_TRUE(d->vex);
    EXPECT_FALSE(d->vex_l);
    EXPECT_EQ(d->vvvv, 1);
    EXPECT_EQ(d->dst.reg, 0);
    EXPECT_EQ(d->src.reg, 2);
    EXPECT_EQ(d->length, 4);
}

TEST(Avx, ThreeByteVexDecode256) {
    // C4 E1 74 58 D1 = vaddps ymm2, ymm1, ymm1... (W0 vvvv=~1 L1 pp=0)
    // bytes: modrm D1 = reg=2, rm=1 -> vaddps ymm2, ymm1, ymm1
    const uint8_t code[] = {0xC4, 0xE1, 0x74, 0x58, 0xD1};
    auto d = cpu::x86::decode(code, sizeof(code));
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->cls, cpu::x86::InstrClass::SseArith);
    EXPECT_EQ(d->sse_op, cpu::x86::SseOp::Add);
    EXPECT_EQ(d->sse_form, cpu::x86::SseForm::PS);
    EXPECT_TRUE(d->vex);
    EXPECT_TRUE(d->vex_l);
    EXPECT_EQ(d->vvvv, 1);
    EXPECT_EQ(d->dst.reg, 2);
    EXPECT_EQ(d->src.reg, 1);
    EXPECT_EQ(d->opsize, 32);
    EXPECT_EQ(d->length, 5);
}

TEST(Avx, VexRxbExtensions) {
    // C4 61 30 58 C2: R=0 X=1 B=1 map=1, W0 vvvv=0110 L0 pp=0
    // -> vaddps xmm8, xmm9, xmm2 (dst and vvvv in the 8..15 range)
    const uint8_t code[] = {0xC4, 0x61, 0x30, 0x58, 0xC2};
    auto d = cpu::x86::decode(code, sizeof(code));
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->dst.reg, 8);
    EXPECT_EQ(d->vvvv, 9);
    EXPECT_EQ(d->src.reg, 2);
}

TEST(Avx, VexWSelectsGprSize) {
    // C4 E1 F2 2A C2 = vcvtsi2ss xmm0, xmm1, rdx (W=1 -> 64-bit GPR source)
    const uint8_t code[] = {0xC4, 0xE1, 0xF2, 0x2A, 0xC2};
    auto d = cpu::x86::decode(code, sizeof(code));
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->cls, cpu::x86::InstrClass::SseCvt);
    EXPECT_EQ(d->sse_op, cpu::x86::SseOp::IntToF32);
    EXPECT_EQ(d->src.size, 8);
}

TEST(Avx, Map0F3AUnsupported) {
    // C4 E3 79 0F xx: VEX map 3 (0F3A) is not implemented.
    const uint8_t code[] = {0xC4, 0xE3, 0x79, 0x0F, 0xC0};
    auto d = cpu::x86::decode(code, sizeof(code));
    ASSERT_FALSE(d.has_value());
    EXPECT_EQ(d.error().code, Status::Unsupported);
}

// ---------- AVX execution (hardware-verified semantics) ----------


TEST(Avx, VaddssUpperFromSrc1AndZeroedYmmPatched) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0x0F, 0x28, 0x0D}, 0x100);   // movaps xmm1, [base+0x100] (SRC1 = vvvv)
    code.insert(code.end(), {
        0xB8, 0x00, 0x00, 0x20, 0x40,                // mov eax, 2.5f
        0x66, 0x0F, 0x6E, modrm(3, 2, 0),            // movd xmm2, eax
        0xC4, 0xE1, 0x72, 0x58, 0xDA,                // vaddss xmm3, xmm1, xmm2
    });
    while (code.size() < 0x100) code.push_back(0x90);
    append_f32(code, 1.5f);
    append_f32(code, 3.0f);
    append_f32(code, 4.0f);
    append_f32(code, 5.0f);
    RunOut out;
    run_guest(out, code, 0x400000, 8);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::MaxSteps);
    EXPECT_FLOAT_EQ(f32_at(out.state, 3, 0), 4.0f);  // 1.5 + 2.5
    EXPECT_FLOAT_EQ(f32_at(out.state, 3, 1), 3.0f);  // DEST[63:32] = SRC1
    EXPECT_FLOAT_EQ(f32_at(out.state, 3, 2), 4.0f);
    EXPECT_FLOAT_EQ(f32_at(out.state, 3, 3), 5.0f);
    // VEX.128 zeroes the YMM upper half of the destination.
    EXPECT_EQ(out.state.ymm_hi[3].lo, 0u);
    EXPECT_EQ(out.state.ymm_hi[3].hi, 0u);
}

TEST(Avx, VaddpsYmm256EightLanes) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x0D}, 0x100); // vmovups ymm1, [A]
    code.insert(code.end(), {
        0xC4, 0xE1, 0x74, 0x58, 0xD1,                // vaddps ymm2, ymm1, ymm1
    });
    while (code.size() < 0x100) code.push_back(0x90);
    for (int i = 0; i < 8; ++i) append_f32(code, static_cast<float>(i + 1)); // A = 1..8
    RunOut out;
    run_guest(out, code, 0x400000, 2);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::MaxSteps);
    // ymm2 = 2 * A lanes: {2,4,6,8,10,12,14,16}
    const float expect[8] = {2, 4, 6, 8, 10, 12, 14, 16};
    for (int i = 0; i < 8; ++i) EXPECT_FLOAT_EQ(y32_at(out.state, 2, i), expect[i]) << i;
    // Also check the raw upper half storage directly.
    EXPECT_NE(out.state.ymm_hi[2].lo, 0u);
}

TEST(Avx, Vaddps128ZeroesYmmUpper) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x05}, 0x100); // vmovups ymm0, [A]
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x0D}, 0x120); // vmovups ymm1, [B]
    code.insert(code.end(), {
        0xC4, 0xE1, 0x70, 0x58, 0xD8,                // vaddps xmm3, xmm1, xmm0 (L=0)
    });
    while (code.size() < 0x100) code.push_back(0x90);
    for (int i = 0; i < 8; ++i) append_f32(code, static_cast<float>(i + 1));   // A
    while (code.size() < 0x120) code.push_back(0x90);
    for (int i = 0; i < 8; ++i) append_f32(code, 100.0f + i);                    // B
    RunOut out;
    run_guest(out, code, 0x400000, 3);
    // 128-bit result: lanes 0..3 = (100+i) + (i+1)
    for (int i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(y32_at(out.state, 3, i), 101.0f + 2 * i) << i;
    // VEX.128 zeroing: upper half cleared even though operands were 256-bit.
    EXPECT_EQ(out.state.ymm_hi[3].lo, 0u);
    EXPECT_EQ(out.state.ymm_hi[3].hi, 0u);
}

TEST(Avx, VsqrtpsPackedAllLanesFromSrc2) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x0D}, 0x120); // vmovups ymm1, [B]
    code.insert(code.end(), {
        0xC4, 0xE1, 0x7C, 0x51, 0xD1,                // vsqrtps ymm2, ymm1
    });
    while (code.size() < 0x120) code.push_back(0x90);
    const float sq[8] = {4, 9, 16, 25, 36, 49, 64, 81};
    for (float v : sq) append_f32(code, v);
    RunOut out;
    run_guest(out, code, 0x400000, 2);
    const float expect[8] = {2, 3, 4, 5, 6, 7, 8, 9};
    for (int i = 0; i < 8; ++i) EXPECT_FLOAT_EQ(y32_at(out.state, 2, i), expect[i]) << i;
}

TEST(Avx, VsqrtssUpperFromSrc1) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0x0F, 0x28, 0x0D}, 0x100);   // movaps xmm1, [A]: {4.0, 11,12,13}
    code.insert(code.end(), {
        0xB8, 0x00, 0x00, 0x10, 0x41,                // mov eax, 9.0f
        0x66, 0x0F, 0x6E, modrm(3, 2, 0),            // movd xmm2, eax
        0xC4, 0xE1, 0x72, 0x51, 0xDA,                // vsqrtss xmm3, xmm1, xmm2
    });
    while (code.size() < 0x100) code.push_back(0x90);
    append_f32(code, 4.0f);
    append_f32(code, 11.0f);
    append_f32(code, 12.0f);
    append_f32(code, 13.0f);
    RunOut out;
    run_guest(out, code, 0x400000, 4);
    EXPECT_FLOAT_EQ(f32_at(out.state, 3, 0), 3.0f);  // sqrt(9)
    EXPECT_FLOAT_EQ(f32_at(out.state, 3, 1), 11.0f); // DEST[63:32] = SRC1
    EXPECT_FLOAT_EQ(f32_at(out.state, 3, 2), 12.0f);
    EXPECT_FLOAT_EQ(f32_at(out.state, 3, 3), 13.0f);
}

TEST(Avx, VmovssMemLoadZeroesAboveLane0) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x05}, 0x100); // vmovups ymm0, [garbage]
    emit_rip_rel(code, {0xC4, 0xE1, 0x7A, 0x10, 0x1D}, 0x130); // vmovss xmm3, [F]
    while (code.size() < 0x100) code.push_back(0x90);
    for (int i = 0; i < 8; ++i) append_f32(code, 7.0f);
    while (code.size() < 0x130) code.push_back(0x90);
    append_f32(code, 2.5f);
    RunOut out;
    run_guest(out, code, 0x400000, 2);
    // Unlike legacy MOVSS, the VEX memory-source form zeroes DEST[127:32].
    EXPECT_EQ(out.state.xmm[3].lo, 0x40200000ULL);
    EXPECT_EQ(out.state.xmm[3].hi, 0u);
    EXPECT_EQ(out.state.ymm_hi[3].lo, 0u);
    EXPECT_EQ(out.state.ymm_hi[3].hi, 0u);
}

TEST(Avx, VmovssRegFormCopiesSrc1Upper) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0x0F, 0x28, 0x05}, 0x100);   // movaps xmm0, [A]
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x0D}, 0x120); // vmovups ymm4, [garbage]
    code.insert(code.end(), {
        0xB8, 0x00, 0x00, 0xC0, 0x3F,                // mov eax, 1.5f
        0x66, 0x0F, 0x6E, modrm(3, 2, 0),            // movd xmm2, eax
        0xC4, 0xE1, 0x7A, 0x10, 0xE2,                // vmovss xmm4, xmm0, xmm2
    });
    while (code.size() < 0x100) code.push_back(0x90);
    append_f32(code, 0.0f);
    append_f32(code, 21.0f);
    append_f32(code, 22.0f);
    append_f32(code, 23.0f);
    while (code.size() < 0x120) code.push_back(0x90);
    for (int i = 0; i < 8; ++i) append_f32(code, 7.0f); // ymm4 seed garbage
    RunOut out;
    run_guest(out, code, 0x400000, 5);
    EXPECT_FLOAT_EQ(f32_at(out.state, 4, 0), 1.5f);  // lane0 = SRC2
    EXPECT_FLOAT_EQ(f32_at(out.state, 4, 1), 21.0f); // DEST[63:32] = SRC1 (xmm0)
    EXPECT_FLOAT_EQ(f32_at(out.state, 4, 2), 22.0f);
    EXPECT_FLOAT_EQ(f32_at(out.state, 4, 3), 23.0f);
    // Register-form VMOVSS also zeroes the upper YMM half (hardware-verified).
    EXPECT_EQ(out.state.ymm_hi[4].lo, 0u);
    EXPECT_EQ(out.state.ymm_hi[4].hi, 0u);
}

TEST(Avx, VmovdqaYmmMisalignedFaults) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0xFD, 0x6F, 0x05}, 0x101); // vmovdqa ymm0, [base+0x101]
    RunOut out;
    run_guest(out, code, 0x400000, 1);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Fault);
}

TEST(Avx, VpadddYmmAndVpsubd) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x05}, 0x100); // vmovups ymm0, [A]
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x0D}, 0x120); // vmovups ymm1, [B]
    code.insert(code.end(), {
        0xC4, 0xE1, 0x75, 0xFE, 0xD0,                // vpaddd ymm2, ymm1, ymm0
        0xC4, 0xE1, 0x6D, 0xFA, 0xD9,                // vpsubd ymm3, ymm2, ymm1 (vvvv=ymm2)
    });
    while (code.size() < 0x100) code.push_back(0x90);
    for (int i = 0; i < 8; ++i) append_i32(code, i);          // A = 0..7
    while (code.size() < 0x120) code.push_back(0x90);
    for (int i = 0; i < 8; ++i) append_i32(code, 10 + i);     // B = 10..17
    RunOut out;
    run_guest(out, code, 0x400000, 4);
    // ymm2 = B + A = 10..17; ymm3 = ymm2 - B = 0..7
    auto u32_at = [](const cpu::CpuState& s, int reg, int lane) -> uint32_t {
        if (lane < 2) return static_cast<uint32_t>(s.xmm[reg].lo >> (32 * lane));
        if (lane < 4) return static_cast<uint32_t>(s.xmm[reg].hi >> (32 * (lane - 2)));
        if (lane < 6) return static_cast<uint32_t>(s.ymm_hi[reg].lo >> (32 * (lane - 4)));
        return static_cast<uint32_t>(s.ymm_hi[reg].hi >> (32 * (lane - 6)));
    };
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(u32_at(out.state, 2, i), static_cast<uint32_t>(10 + 2 * i));
    for (int i = 0; i < 8; ++i) EXPECT_EQ(u32_at(out.state, 3, i), static_cast<uint32_t>(i));
}

TEST(Avx, Vpmuludq256FourProducts) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x05}, 0x100); // vmovups ymm0, [A]
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x0D}, 0x120); // vmovups ymm1, [B]
    code.insert(code.end(), {
        0xC4, 0xE1, 0x75, 0xF4, 0xD0,                // vpmuludq ymm2, ymm1, ymm0
    });
    while (code.size() < 0x100) code.push_back(0x90);
    const uint32_t a[8] = {1, 99, 3, 99, 5, 99, 7, 99};       // even lanes used
    const uint32_t b[8] = {10, 88, 30, 88, 50, 88, 70, 88};
    for (uint32_t v : a) append_i32(code, static_cast<int32_t>(v));
    while (code.size() < 0x120) code.push_back(0x90);
    for (uint32_t v : b) append_i32(code, static_cast<int32_t>(v));
    RunOut out;
    run_guest(out, code, 0x400000, 3);
    const uint64_t expect[4] = {10, 90, 250, 490};
    const uint64_t got[4] = {
        out.state.xmm[2].lo, out.state.xmm[2].hi,
        out.state.ymm_hi[2].lo, out.state.ymm_hi[2].hi,
    };
    for (int i = 0; i < 4; ++i) EXPECT_EQ(got[i], expect[i]) << i;
}

TEST(Avx, Vpshufd256IndependentHalves) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x05}, 0x100); // vmovups ymm0, [A]
    code.insert(code.end(), {
        0xC4, 0xE1, 0xFD, 0x70, 0xC8, 0x1B,          // vpshufd ymm1, ymm0, 0x1B
    });
    while (code.size() < 0x100) code.push_back(0x90);
    for (int i = 0; i < 8; ++i) append_i32(code, 10 + i);     // 10..17
    RunOut out;
    run_guest(out, code, 0x400000, 2);
    auto u32_at = [](const cpu::CpuState& s, int reg, int lane) -> uint32_t {
        if (lane < 2) return static_cast<uint32_t>(s.xmm[reg].lo >> (32 * lane));
        if (lane < 4) return static_cast<uint32_t>(s.xmm[reg].hi >> (32 * (lane - 2)));
        if (lane < 6) return static_cast<uint32_t>(s.ymm_hi[reg].lo >> (32 * (lane - 4)));
        return static_cast<uint32_t>(s.ymm_hi[reg].hi >> (32 * (lane - 6)));
    };
    // 0x1B reverses within each half: {13,12,11,10, 17,16,15,14}
    const uint32_t expect[8] = {13, 12, 11, 10, 17, 16, 15, 14};
    for (int i = 0; i < 8; ++i) EXPECT_EQ(u32_at(out.state, 1, i), expect[i]) << i;
}

TEST(Avx, Vcvtsi2ssUsesVvvvUpper) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0x0F, 0x28, 0x0D}, 0x100);   // movaps xmm1, [A]
    code.insert(code.end(), {
        0xB8, 0x39, 0x30, 0x00, 0x00,                // mov eax, 12345
        0xC4, 0xE1, 0x72, 0x2A, 0xD0,                // vcvtsi2ss xmm2, xmm1, eax
    });
    while (code.size() < 0x100) code.push_back(0x90);
    append_f32(code, 0.0f);
    append_f32(code, 31.0f);
    append_f32(code, 32.0f);
    append_f32(code, 33.0f);
    RunOut out;
    run_guest(out, code, 0x400000, 3);
    EXPECT_FLOAT_EQ(f32_at(out.state, 2, 0), 12345.0f);
    EXPECT_FLOAT_EQ(f32_at(out.state, 2, 1), 31.0f); // DEST[63:32] = SRC1
    EXPECT_FLOAT_EQ(f32_at(out.state, 2, 2), 32.0f);
    EXPECT_FLOAT_EQ(f32_at(out.state, 2, 3), 33.0f);
    EXPECT_EQ(out.state.ymm_hi[2].lo, 0u);           // VEX.128 zeroing
}

TEST(Avx, Vcvtdq2ps256) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x05}, 0x100); // vmovups ymm0, [A]
    code.insert(code.end(), {
        0xC4, 0xE1, 0x7C, 0x5B, 0xC8,                // vcvtdq2ps ymm1, ymm0
    });
    while (code.size() < 0x100) code.push_back(0x90);
    const int32_t in[8] = {-3, 7, 1000000, -1000000, 2, 4, 6, 8};
    for (int32_t v : in) append_i32(code, v);
    RunOut out;
    run_guest(out, code, 0x400000, 2);
    const float expect[8] = {-3.0f, 7.0f, 1000000.0f, -1000000.0f, 2.0f, 4.0f, 6.0f, 8.0f};
    for (int i = 0; i < 8; ++i) EXPECT_FLOAT_EQ(y32_at(out.state, 1, i), expect[i]) << i;
}

TEST(Avx, VucomissFlagsPatched) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0x0F, 0x28, 0x05}, 0x100);   // movaps xmm0, [A] (1.0f,...)
    emit_rip_rel(code, {0x0F, 0x28, 0x0D}, 0x110);   // movaps xmm1, [B] (2.0f,...)
    code.insert(code.end(), {
        0xC4, 0xE1, 0x78, 0x2F, 0xC1,                // vucomiss xmm0, xmm1 (1 < 2)
    });
    while (code.size() < 0x100) code.push_back(0x90);
    append_f32(code, 1.0f);
    append_f32(code, 0.0f);
    append_f32(code, 0.0f);
    append_f32(code, 0.0f);
    while (code.size() < 0x110) code.push_back(0x90);
    append_f32(code, 2.0f);
    RunOut out;
    run_guest(out, code, 0x400000, 3);
    EXPECT_TRUE(out.state.flags.cf);
    EXPECT_FALSE(out.state.flags.zf);
}

TEST(Avx, VandnpsVxorps256Patched) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x05}, 0x100); // vmovups ymm0, [A]
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x0D}, 0x120); // vmovups ymm1, [B]
    code.insert(code.end(), {
        0xC4, 0xE1, 0x74, 0x55, 0xD1,                // vandnps ymm2, ymm1, ymm0
        0xC4, 0xE1, 0x74, 0x57, 0xD9,                // vxorps ymm3, ymm1, ymm1
    });
    while (code.size() < 0x100) code.push_back(0x90);
    // A: 0x3F800000 pattern via raw bits: use append_i32 with the bit pattern
    for (int i = 0; i < 8; ++i) append_i32(code, static_cast<int32_t>(0x3F800000u));
    while (code.size() < 0x120) code.push_back(0x90);
    for (int i = 0; i < 8; ++i) append_i32(code, static_cast<int32_t>(0x3F000000u));
    RunOut out;
    run_guest(out, code, 0x400000, 4);
    // vandnps: NOT(0x3F000000) AND 0x3F800000 = 0xC07FFFFF & 0x3F800000 = 0
    auto u32_at = [](const cpu::CpuState& s, int reg, int lane) -> uint32_t {
        if (lane < 2) return static_cast<uint32_t>(s.xmm[reg].lo >> (32 * lane));
        if (lane < 4) return static_cast<uint32_t>(s.xmm[reg].hi >> (32 * (lane - 2)));
        if (lane < 6) return static_cast<uint32_t>(s.ymm_hi[reg].lo >> (32 * (lane - 4)));
        return static_cast<uint32_t>(s.ymm_hi[reg].hi >> (32 * (lane - 6)));
    };
    for (int i = 0; i < 8; ++i) EXPECT_EQ(u32_at(out.state, 2, i), 0u) << i;
    // vxorps ymm3, ymm1, ymm1 -> zero
    for (int i = 0; i < 8; ++i) EXPECT_EQ(u32_at(out.state, 3, i), 0u) << i;
}

TEST(Avx, VcmppsYmmEqPredicate) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x05}, 0x100); // vmovups ymm0, [A]
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x0D}, 0x120); // vmovups ymm1, [B]
    code.insert(code.end(), {
        0xC4, 0xE1, 0x74, 0xC2, 0xD0, 0x00,          // vcmpps ymm2, ymm1, ymm0, EQ
    });
    while (code.size() < 0x100) code.push_back(0x90);
    const float a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    for (float v : a) append_f32(code, v);
    while (code.size() < 0x120) code.push_back(0x90);
    const float b[8] = {1, 9, 3, 9, 5, 9, 7, 9};
    for (float v : b) append_f32(code, v);
    RunOut out;
    run_guest(out, code, 0x400000, 3);
    auto u32_at = [](const cpu::CpuState& s, int reg, int lane) -> uint32_t {
        if (lane < 2) return static_cast<uint32_t>(s.xmm[reg].lo >> (32 * lane));
        if (lane < 4) return static_cast<uint32_t>(s.xmm[reg].hi >> (32 * (lane - 2)));
        if (lane < 6) return static_cast<uint32_t>(s.ymm_hi[reg].lo >> (32 * (lane - 4)));
        return static_cast<uint32_t>(s.ymm_hi[reg].hi >> (32 * (lane - 6)));
    };
    const uint32_t expect[8] = {0xFFFFFFFFu, 0, 0xFFFFFFFFu, 0,
                                0xFFFFFFFFu, 0, 0xFFFFFFFFu, 0};
    for (int i = 0; i < 8; ++i) EXPECT_EQ(u32_at(out.state, 2, i), expect[i]) << i;
}

TEST(Avx, VmovdZeroFillsAllAbove) {
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x05}, 0x100); // vmovups ymm3, [garbage]
    code.insert(code.end(), {
        0xB8, 0x21, 0x43, 0x65, 0x87,                // mov eax, 0x87654321
        0xC4, 0xE1, 0x79, 0x6E, 0xD8,                // vmovd xmm3, eax
    });
    while (code.size() < 0x100) code.push_back(0x90);
    for (int i = 0; i < 8; ++i) append_f32(code, 7.0f);
    RunOut out;
    run_guest(out, code, 0x400000, 3);
    EXPECT_EQ(out.state.xmm[3].lo, 0x87654321ULL);
    EXPECT_EQ(out.state.xmm[3].hi, 0u);
    EXPECT_EQ(out.state.ymm_hi[3].lo, 0u);
    EXPECT_EQ(out.state.ymm_hi[3].hi, 0u);
}

TEST(Avx, UnsupportedVexOpcodeStopsRun) {
    // C4 E1 7C 5F D1 = vmaxps ymm2, ymm0, ymm1 — not implemented; the run
    // must stop with Unsupported instead of mis-executing.
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x05}, 0x100); // vmovups ymm0, [A]
    code.insert(code.end(), {
        0xC4, 0xE1, 0x7C, 0x5F, 0xD1,                // vmaxps ymm2, ymm0, ymm1
    });
    RunOut out;
    run_guest(out, code, 0x400000, 10);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Unsupported);
}

TEST(Avx, EndToEndProgramExitsCleanly) {
    // vmovups ymm0,[A]; vcvtdq2ps ymm1, ymm0; vaddps ymm2, ymm1, ymm1;
    // vmovups [out], ymm2; exit(0). Exercises load -> compute -> store.
    std::vector<uint8_t> code;
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x10, 0x05}, 0x100); // vmovups ymm0, [A]
    code.insert(code.end(), {
        0xC4, 0xE1, 0x7C, 0x5B, 0xC8,                // vcvtdq2ps ymm1, ymm0
        0xC4, 0xE1, 0x74, 0x58, 0xD1,                // vaddps ymm2, ymm1, ymm1
    });
    emit_rip_rel(code, {0xC4, 0xE1, 0x7C, 0x11, 0x15}, 0x140); // vmovups [out], ymm2
    code.insert(code.end(), {
        0xBF, 0x00, 0x00, 0x00, 0x00,                // mov edi, 0
        0xB8, 0x01, 0x00, 0x00, 0x00,                // mov eax, 1 (SYS_exit)
        0x0F, 0x05,                                  // syscall
    });
    while (code.size() < 0x100) code.push_back(0x90);
    for (int i = 0; i < 8; ++i) append_i32(code, i);          // A = 0..7
    while (code.size() < 0x140) code.push_back(0x90);
    RunOut out;
    run_guest(out, code);
    EXPECT_EQ(out.stats.stop, cpu::RunStats::Stop::Exited);
    EXPECT_EQ(out.stats.exit_code, 0u);
    const float expect[8] = {0, 2, 4, 6, 8, 10, 12, 14};
    for (int i = 0; i < 8; ++i) EXPECT_FLOAT_EQ(y32_at(out.state, 2, i), expect[i]) << i;
}
