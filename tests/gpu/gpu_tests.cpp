// GPU-layer tests: PM4 decoding, resource registry, RDNA2 formats, Vulkan probe.
#include <gtest/gtest.h>

#include "armsx5/gpu/pm4/pm4.h"
#include "armsx5/gpu/resource.h"
#include "armsx5/gpu/shader/rdna2.h"
#include "armsx5/gpu/shader/spirv.h"
#include "armsx5/gpu/vulkan/device_info.h"

#include <cstdint>
#include <span>
#include <vector>

using namespace armsx5;
using namespace armsx5::gpu;

// ---------- PM4 ----------
TEST(Pm4, DecodesSetContextReg) {
    const std::vector<uint32_t> buf = {
        (3u << 30) | (2u << 16) | (0x69u << 8),
        0xA2A0, // reg offset
        0x12345678,
    };
    auto out = pm4::decode_type3(buf.data(), buf.size());
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->packets.size(), 1u);
    EXPECT_EQ(out->packets[0].opcode, 0x69u);
    EXPECT_STREQ(out->packets[0].name(), "SET_CONTEXT_REG");
    EXPECT_EQ(out->packets[0].count, 2u);
    ASSERT_EQ(out->packets[0].body.size(), 2u);
    EXPECT_EQ(out->packets[0].body[1], 0x12345678u);
    EXPECT_EQ(out->unknown_opcode_count, 0u);
}

TEST(Pm4, DecodesMultiplePackets) {
    const std::vector<uint32_t> buf = {
        (3u << 30) | (0u << 16) | (0x10u << 8), // NOP, no body
        (3u << 30) | (1u << 16) | (0x2Du << 8), // DRAW_INDEX_AUTO, 1 body dword
        100,
    };
    auto out = pm4::decode_type3(buf.data(), buf.size());
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->packets.size(), 2u);
    EXPECT_STREQ(out->packets[0].name(), "NOP");
    EXPECT_STREQ(out->packets[1].name(), "DRAW_INDEX_AUTO");
    EXPECT_EQ(out->packets[1].body[0], 100u);
}

TEST(Pm4, UnknownOpcodeRecordedNotFabricated) {
    const std::vector<uint32_t> buf = {
        (3u << 30) | (0u << 16) | (0xEAu << 8), // undocumented opcode
    };
    auto out = pm4::decode_type3(buf.data(), buf.size());
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->packets[0].opcode, 0xEAu);
    EXPECT_EQ(out->packets[0].name(), nullptr);
    EXPECT_EQ(out->unknown_opcode_count, 1u);
}

TEST(Pm4, RejectsNonType3) {
    const std::vector<uint32_t> buf = {2u << 30}; // type 2
    auto out = pm4::decode_type3(buf.data(), buf.size());
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Status::Unsupported);
}

TEST(Pm4, RejectsTruncatedPacket) {
    const std::vector<uint32_t> buf = {
        (3u << 30) | (3u << 16) | (0x69u << 8),
        0xA2A0, // only 1 of 3 body dwords present
    };
    auto out = pm4::decode_type3(buf.data(), buf.size());
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Status::ParseError);
}

TEST(Pm4, RejectsEmptyBuffer) {
    auto out = pm4::decode_type3(nullptr, 0);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Status::InvalidArgument);
}

// ---------- resources ----------
TEST(Resources, RegisterAndLookupBuffer) {
    ResourceRegistry reg;
    ASSERT_TRUE(reg.register_buffer(0x10000, 0x4000).has_value());
    const auto* b = reg.find_buffer(0x10000);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->size, 0x4000u);
    EXPECT_EQ(b->residency, Residency::Resident);
    EXPECT_EQ(reg.find_buffer(0x20000), nullptr);
}

TEST(Resources, RejectsOverlappingBuffers) {
    ResourceRegistry reg;
    ASSERT_TRUE(reg.register_buffer(0x10000, 0x4000).has_value());
    auto r = reg.register_buffer(0x12000, 0x1000);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Status::InvalidArgument);
}

TEST(Resources, ResidencyTransitions) {
    ResourceRegistry reg;
    ASSERT_TRUE(reg.register_buffer(0x30000, 0x1000).has_value());
    reg.set_residency(0x30000, Residency::Evicted);
    EXPECT_EQ(reg.evicted_count(), 1u);
    reg.set_residency(0x30000, Residency::Resident);
    EXPECT_EQ(reg.evicted_count(), 0u);
}

TEST(Resources, RegisterImageAndRejectOverlap) {
    ResourceRegistry reg;
    ASSERT_TRUE(reg.register_image(0x40000, 64, 64, 1).has_value());
    auto r = reg.register_buffer(0x41000, 0x1000);
    ASSERT_FALSE(r.has_value()); // image occupies 64*64*4 = 0x4000 bytes
    EXPECT_EQ(r.error().code, Status::InvalidArgument);
}

// ---------- RDNA2 format decode ----------
// All reference encodings below are literal GFX10/10.3 byte sequences from
// LLVM's MC test suite (llvm/test/MC/AMDGPU/gfx10_asm_*.s), which are
// validated against real RDNA hardware by the AMD toolchain.

TEST(Rdna2, Vop2AddF32Encoding) {
    // v_add_f32_e32 v5, v1, v2  ->  [0x01,0x05,0x0a,0x06]
    const uint32_t word = 0x060A0501u;
    auto r = rdna2::decode(word);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->format, rdna2::Format::VOP2);
    EXPECT_EQ(r->opcode, 3u); // V_ADD_F32
    EXPECT_EQ(r->dst, 5u);
    EXPECT_EQ(r->src0, 0x101u); // bit8=VGPR select | v1
    EXPECT_TRUE(r->src0_is_vgpr);
    EXPECT_EQ(r->src1, 2u); // e32 src1 is the 8-bit VGPR-only field
    EXPECT_FALSE(rdna2::format_is_8_byte(r->format));
}

TEST(Rdna2, Vop2ScalarAndConstSrc0) {
    // v_add_f32_e32 v5, s101, v2  ->  [0x65,0x04,0x0a,0x06]
    auto r = rdna2::decode(0x060A0465u);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->src0, 101u);
    EXPECT_FALSE(r->src0_is_vgpr);

    // v_add_f32_e32 v5, 0.5, v2  ->  [0xf0,0x04,0x0a,0x06]
    auto c = rdna2::decode(0x060A04F0u);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->src0, 0xF0u); // float-constant table slot 0 (0.5)
    EXPECT_FALSE(c->src0_is_vgpr);
}

TEST(Rdna2, Vop2LiteralSrc0IsTwoWord) {
    // v_add_f32_e32 v5, 0xaf123456, v2
    //   ->  [0xff,0x04,0x0a,0x06, 0x56,0x34,0x12,0xaf]
    const uint32_t words[2] = {0x060A04FFu, 0xAF123456u};
    auto r = rdna2::decode(std::span<const uint32_t>(words, 2));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->literal_src);
    EXPECT_TRUE(r->two_word);
    EXPECT_EQ(r->imm32, 0xAF123456u);
}

TEST(Rdna2, Vop1MovB32Encoding) {
    // v_mov_b32_e32 v5, v1  ->  [0x01,0x03,0x0a,0x7e]
    const uint32_t word = 0x7E0A0301u;
    auto r = rdna2::decode(word);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->format, rdna2::Format::VOP1);
    EXPECT_EQ(r->opcode, 1u); // V_MOV_B32
    EXPECT_EQ(r->dst, 5u);
    EXPECT_EQ(r->src0, 0x101u);
}

TEST(Rdna2, Vop1NopEncoding) {
    // v_nop_e32  ->  [0x00,0x00,0x00,0x7e]
    auto r = rdna2::decode(0x7E000000u);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->format, rdna2::Format::VOP1);
    EXPECT_EQ(r->opcode, 0u);
}

TEST(Rdna2, Vop3E64AddF32) {
    // v_add_f32_e64 v5, v1, v2
    //   ->  [0x05,0x00,0x03,0xd5, 0x01,0x05,0x02,0x02]
    const uint32_t words[2] = {0xD5030005u, 0x02020501u};
    auto r = rdna2::decode(std::span<const uint32_t>(words, 2));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->format, rdna2::Format::VOP3);
    EXPECT_EQ(r->opcode, 0x103u); // V_ADD_F32 in the 0x100 VOP2-class range
    EXPECT_EQ(r->dst, 5u);
    EXPECT_EQ(r->src0, 0x101u); // 9-bit src0: VGPR select | v1
    EXPECT_EQ(r->src1, 0x102u); // 9-bit src1: VGPR select | v2
    EXPECT_TRUE(r->two_word);
    EXPECT_TRUE(rdna2::format_is_8_byte(r->format));
}

TEST(Rdna2, Sop1MovB32Encoding) {
    // s_mov_b32 s1, s2  ->  [0x02,0x03,0x81,0xbe]
    const uint32_t word = 0xBE810302u;
    auto r = rdna2::decode(word);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->format, rdna2::Format::SOP1);
    EXPECT_EQ(r->opcode, 3u); // GFX10 moved S_MOV_B32 to 0x03
    EXPECT_EQ(r->dst, 1u);
    EXPECT_EQ(r->src0, 2u);
}

TEST(Rdna2, Sop1LiteralSource) {
    // s_mov_b32 s1, 0x64  ->  [0xff,0x03,0x81,0xbe, 0x64,0x00,0x00,0x00]
    const uint32_t words[2] = {0xBE8103FFu, 0x64u};
    auto r = rdna2::decode(std::span<const uint32_t>(words, 2));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->literal_src);
    EXPECT_TRUE(r->two_word);
    EXPECT_EQ(r->imm32, 0x64u);
}

TEST(Rdna2, Sop2AndB32Encoding) {
    // s_and_b32 s2, s4, s6  ->  [0x04,0x06,0x02,0x87]
    const uint32_t word = 0x87020604u;
    auto r = rdna2::decode(word);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->format, rdna2::Format::SOP2);
    EXPECT_EQ(r->opcode, 14u); // GFX10 S_AND_B32 = 14 (GFX8 used 6)
    EXPECT_EQ(r->dst, 2u);
    EXPECT_EQ(r->src0, 4u);
    EXPECT_EQ(r->src1, 6u);
}

TEST(Rdna2, SopkMovkAndCall) {
    // s_movk_i32 s0, 0x64 -> [31:28]=0xb, op[27:23]=0, sdst[22:16]=0, imm=0x64
    auto r = rdna2::decode(0xB0000064u);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->format, rdna2::Format::SOPK);
    EXPECT_EQ(r->opcode, 0u); // S_MOVK_I32
    EXPECT_EQ(r->dst, 0u);
    EXPECT_EQ(r->imm16, 0x64u);

    // s_call_b64 null, 12609  ->  [0x41,0x31,0x7d,0xbb] (LLVM MC sample)
    auto c = rdna2::decode(0xBB7D3141u);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->format, rdna2::Format::SOPK);
    EXPECT_EQ(c->opcode, 22u); // S_CALL_B64 (GFX10)
    EXPECT_EQ(c->dst, 125u);   // "null"
    EXPECT_EQ(c->imm16, 12609u);
}

TEST(Rdna2, SoppEndpgm) {
    // s_endpgm 0 -> [31:23]=0x17f, op[22:16]=1
    auto r = rdna2::decode(0xBF810000u);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->format, rdna2::Format::SOPP);
    EXPECT_EQ(r->opcode, 1u);
    EXPECT_EQ(r->imm16, 0u);
}

TEST(Rdna2, SopcAndSoppFamilySplit) {
    // SOPC: [31:23]=0x17e (v=62) vs SOPP: [31:23]=0x17f (v=63)
    auto c = rdna2::decode(0xBF000602u); // s_cmp_eq_u32 s2, s2-ish shape
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->format, rdna2::Format::SOPC);
    auto p = rdna2::decode(0xBF810000u);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->format, rdna2::Format::SOPP);
}

TEST(Rdna2, ExpMrt0FullMask) {
    // exp mrt0 v0, v0, v0, v0 -> [0x0f,0x00,0x00,0xf8, 0x00,0x00,0x00,0x00]
    const uint32_t words[2] = {0xF800000Fu, 0x00000000u};
    auto r = rdna2::decode(std::span<const uint32_t>(words, 2));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->format, rdna2::Format::EXP);
    EXPECT_EQ(r->en, 0xFu);
    EXPECT_EQ(r->target, 0u); // MRT0
    EXPECT_FALSE(r->compr);
    EXPECT_FALSE(r->done);
    EXPECT_TRUE(rdna2::format_is_8_byte(r->format));
}

TEST(Rdna2, ExpPos0Done) {
    // exp pos0, v0, v0, v0, v0 done -> [0xcf,0x08,0x00,0xf8, ...]
    // EN=[3:0], TARGET=[9:4] (pos0 = 12), DONE=[11]
    const uint32_t words[2] = {0xF80008CFu, 0x00000000u};
    auto r = rdna2::decode(std::span<const uint32_t>(words, 2));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->target, 12u); // POS0
    EXPECT_TRUE(r->done);
    EXPECT_EQ(r->en, 0xFu);
}

TEST(Rdna2, ExpDoneAndSrcFields) {
    // exp mrt0 off, off, off, v1 done
    //   ->  [0x08,0x08,0x00,0xf8, 0x00,0x00,0x00,0x01]
    const uint32_t words[2] = {0xF8000808u, 0x01000000u};
    auto r = rdna2::decode(std::span<const uint32_t>(words, 2));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->en, 0x8u); // only src3 enabled
    EXPECT_TRUE(r->done);
    EXPECT_EQ(r->dst, 1u); // src3 lives in word1[31:24]
}

TEST(Rdna2, ExpPartialEnable) {
    // exp mrt0 v0, off, off, off -> EN = 0x1
    const uint32_t words[2] = {0xF8000001u, 0x00000000u};
    auto r = rdna2::decode(std::span<const uint32_t>(words, 2));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->en, 0x1u);
    EXPECT_EQ(r->target, 0u);
}

TEST(Rdna2, MemoryFamiliesClassified) {
    // Prefix bytes observed in gfx10_asm_{smem,ds,flat,mubuf,mimg}.s:
    // SMEM 0xF4 (top6=61), DS 0xD8 (54), FLAT 0xDC (55), MUBUF 0xE0 (56),
    // MIMG 0xF0 (60).
    EXPECT_EQ(rdna2::decode(0xF4010000u)->format, rdna2::Format::SMEM);
    EXPECT_EQ(rdna2::decode(0xD8060000u)->format, rdna2::Format::DS);
    EXPECT_EQ(rdna2::decode(0xDC020000u)->format, rdna2::Format::FLAT);
    EXPECT_EQ(rdna2::decode(0xE0060000u)->format, rdna2::Format::MUBUF);
    EXPECT_EQ(rdna2::decode(0xF0000000u)->format, rdna2::Format::MIMG);
}

TEST(Rdna2, ReservedEncodingRejected) {
    // 0xC4.. was the GFX8/9 EXP prefix (top6=49) and is reserved on GFX10.
    auto r = rdna2::decode(0xC4000000u);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Status::Unsupported);
}

TEST(Rdna2, TruncatedTwoWordRejected) {
    const uint32_t word = 0xF800000Fu; // EXP without its second dword
    auto r = rdna2::decode(word);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Status::InvalidArgument);
}

// ---------- RDNA2 opcode semantic table ----------
TEST(Rdna2Table, Vop2Entries) {
    auto* add = rdna2::lookup_op(rdna2::Format::VOP2, 0x03);
    ASSERT_NE(add, nullptr);
    EXPECT_STREQ(add->name, "V_ADD_F32");
    EXPECT_EQ(add->cls, rdna2::OpClass::VectorFloat);
    EXPECT_TRUE(add->float_op);
    EXPECT_TRUE(add->translatable);

    auto* band = rdna2::lookup_op(rdna2::Format::VOP2, 0x1B);
    ASSERT_NE(band, nullptr);
    EXPECT_STREQ(band->name, "V_AND_B32");
    EXPECT_EQ(band->cls, rdna2::OpClass::VectorInt);
    EXPECT_FALSE(band->float_op);

    auto* fmac = rdna2::lookup_op(rdna2::Format::VOP2, 0x2B);
    ASSERT_NE(fmac, nullptr);
    EXPECT_STREQ(fmac->name, "V_FMAC_F32");
    EXPECT_FALSE(fmac->translatable); // needs accumulate semantics

    EXPECT_EQ(rdna2::lookup_op(rdna2::Format::VOP2, 0x02), nullptr);
}

TEST(Rdna2Table, SopEntries) {
    auto* mov = rdna2::lookup_op(rdna2::Format::SOP1, 0x03);
    ASSERT_NE(mov, nullptr);
    EXPECT_STREQ(mov->name, "S_MOV_B32");
    EXPECT_TRUE(mov->translatable);

    auto* endpgm = rdna2::lookup_op(rdna2::Format::SOPP, 0x01);
    ASSERT_NE(endpgm, nullptr);
    EXPECT_STREQ(endpgm->name, "S_ENDPGM");
    EXPECT_EQ(endpgm->cls, rdna2::OpClass::ProgramEnd);

    auto* add = rdna2::lookup_op(rdna2::Format::SOP2, 0x00);
    ASSERT_NE(add, nullptr);
    EXPECT_STREQ(add->name, "S_ADD_U32"); // GFX10 name (renamed s_add_co_u32)
    EXPECT_TRUE(add->translatable);

    auto* movk = rdna2::lookup_op(rdna2::Format::SOPK, 0x00);
    ASSERT_NE(movk, nullptr);
    EXPECT_STREQ(movk->name, "S_MOVK_I32");

    auto* swappc = rdna2::lookup_op(rdna2::Format::SOP1, 0x21);
    ASSERT_NE(swappc, nullptr);
    EXPECT_STREQ(swappc->name, "S_SWAPPC_B64");
    EXPECT_FALSE(swappc->translatable); // flow control: v1 defers

    // SOP1 opcodes 0-2 are reserved on GFX10 (moved to SOPP/SOPK numbering).
    EXPECT_EQ(rdna2::lookup_op(rdna2::Format::SOP1, 0x00), nullptr);
}

TEST(Rdna2Table, Vop3Entries) {
    auto* add = rdna2::lookup_op(rdna2::Format::VOP3, 0x103);
    ASSERT_NE(add, nullptr);
    EXPECT_STREQ(add->name, "V_ADD_F32"); // 0x100 + VOP2 opcode

    auto* fma = rdna2::lookup_op(rdna2::Format::VOP3, 0x14B);
    ASSERT_NE(fma, nullptr);
    EXPECT_STREQ(fma->name, "V_FMA_F32");
    EXPECT_EQ(fma->src_count, 3u);
    EXPECT_TRUE(fma->translatable);

    auto* rl = rdna2::lookup_op(rdna2::Format::VOP3, 0x360);
    ASSERT_NE(rl, nullptr);
    EXPECT_STREQ(rl->name, "V_READLANE_B32");
    EXPECT_FALSE(rl->translatable);
}

TEST(Rdna2Table, ScalarOperandEncoding) {
    using K = rdna2::Operand::Kind;
    auto sgpr = rdna2::decode_scalar_operand(101);
    EXPECT_EQ(sgpr.kind, K::Sgpr);
    EXPECT_EQ(sgpr.index, 101u);
    EXPECT_EQ(rdna2::decode_scalar_operand(106).kind, K::Vcc);
    EXPECT_EQ(rdna2::decode_scalar_operand(124).kind, K::M0);
    EXPECT_EQ(rdna2::decode_scalar_operand(125).kind, K::Null);
    auto zero = rdna2::decode_scalar_operand(128);
    EXPECT_EQ(zero.kind, K::UintConst);
    EXPECT_EQ(zero.index, 0u);
    auto neg1 = rdna2::decode_scalar_operand(193);
    EXPECT_EQ(neg1.kind, K::IntConst);
    EXPECT_EQ(neg1.index, 1u);
    auto half = rdna2::decode_scalar_operand(240);
    EXPECT_EQ(half.kind, K::FloatConst);
    EXPECT_EQ(rdna2::decode_scalar_operand(255).kind, K::Literal);
    EXPECT_FLOAT_EQ(rdna2::float_const_table()[0], 0.5f);
    EXPECT_FLOAT_EQ(rdna2::float_const_table()[7], -4.0f);
}

// ---------- SPIR-V emitter ----------
TEST(Spirv, PassthroughExportModule) {
    // Input location 0 seeds v0-v3; EXP MRT0 v0,v1,v2,v3 stores them.
    const uint32_t code[2] = {0xF800000Fu, 0x03020100u};
    spirv::ShaderInterface iface;
    iface.input_locations = 1;
    iface.output_locations = 1;
    iface.sgpr_push_count = 4;
    auto out = spirv::emit_fragment(std::span<const uint32_t>(code, 2), iface);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->stats.decoded, 1u);
    EXPECT_EQ(out->stats.translated, 1u);
    EXPECT_EQ(out->stats.skipped, 0u);
    EXPECT_GT(out->id_bound, 1u);
    EXPECT_EQ(out->words[0], spirv::kSpirvMagic);

    auto info = spirv::inspect(out->words);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->version_major, 1u);
    EXPECT_EQ(info->version_minor, 0u);
    EXPECT_TRUE(info->entry_point_fragment);
    EXPECT_TRUE(info->memory_model_logical_glsl450);
    EXPECT_TRUE(info->has_glsl_ext);
    EXPECT_EQ(info->function_count, 1u);
    EXPECT_EQ(info->label_count, 1u);
    EXPECT_EQ(info->return_count, 1u);
    ASSERT_EQ(info->input_locations.size(), 1u);
    EXPECT_EQ(info->input_locations[0], 0u);
    ASSERT_EQ(info->output_locations.size(), 1u);
    EXPECT_EQ(info->output_locations[0], 0u);
    EXPECT_TRUE(info->push_constant_block);
    EXPECT_TRUE(info->errors.empty());
}

TEST(Spirv, FloatArithmeticChain) {
    // v_add_f32 v2, v0, v1   -> 0x06040301
    // v_mul_f32 v3, v2, v2   -> 0x10060502
    // exp mrt0 v3, v2, v0, v0 -> words 0xF800000F, 0x00000203
    const uint32_t code[] = {0x06040301u, 0x10060502u, 0xF800000Fu, 0x00000203u};
    spirv::ShaderInterface iface;
    auto out = spirv::emit_fragment(std::span<const uint32_t>(code, 4), iface);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->stats.translated, 3u);

    auto info = spirv::inspect(out->words);
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->body_opcodes.size(), 2u);
    EXPECT_EQ(info->body_opcodes[0], 129u); // OpFAdd
    EXPECT_EQ(info->body_opcodes[1], 133u); // OpFMul
    EXPECT_STREQ(spirv::opcode_name(129), "OpFAdd");
    EXPECT_STREQ(spirv::opcode_name(133), "OpFMul");
}

TEST(Spirv, IntegerOpsAndScalarArith) {
    // v_add_nc_u32 v4, v0, v1 -> 0x4C040301 (op 0x25)
    // v_and_b32 v5, v4, v0    -> 0x36060401?? (constructed below)
    // s_movk_i32 s0, 100      -> 0xB0000064
    // s_add_u32 s1, s0, s0    -> 0x80010000
    const uint32_t add_u32 = (0x25u << 25) | (4u << 17) | (1u << 9) | 0x101u;
    const uint32_t and_b32 = (0x1Bu << 25) | (5u << 17) | (0u << 9) | (4u | 0x100u);
    const uint32_t code[] = {add_u32, and_b32, 0xB0000064u, 0x80010000u};
    spirv::ShaderInterface iface;
    iface.sgpr_push_count = 8;
    auto out = spirv::emit_fragment(std::span<const uint32_t>(code, 4), iface);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->stats.translated, 4u);

    auto info = spirv::inspect(out->words);
    ASSERT_TRUE(info.has_value());
    // Seeded inputs are f32, so the integer ALU inserts OpBitcasts:
    // add: bc,bc,IAdd; and: bc,And; s_add: IAdd (s0 comes from S_MOVK).
    ASSERT_EQ(info->body_opcodes.size(), 6u);
    EXPECT_EQ(info->body_opcodes[0], 124u); // OpBitcast
    EXPECT_EQ(info->body_opcodes[1], 124u);
    EXPECT_EQ(info->body_opcodes[2], 128u); // OpIAdd
    EXPECT_EQ(info->body_opcodes[3], 124u);
    EXPECT_EQ(info->body_opcodes[4], 199u); // OpBitwiseAnd
    EXPECT_EQ(info->body_opcodes[5], 128u);
}

TEST(Spirv, Vop3FmaUsesExtInst) {
    // v_fma_f32 v4, v0, v1, v2 (e64):
    //   word0 = (0x35<<26) | (0x14B<<16) | 4 = 0xD54B0004
    //   word1 = src2(0x102)<<18 | src1(0x102)<<9 | src0(0x101) = 0x040A0501
    const uint32_t code[] = {0xD54B0004u, 0x040A0501u};
    spirv::ShaderInterface iface;
    auto out = spirv::emit_fragment(std::span<const uint32_t>(code, 2), iface);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->stats.translated, 1u);
    auto info = spirv::inspect(out->words);
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->body_opcodes.size(), 1u);
    EXPECT_EQ(info->body_opcodes[0], 12u); // OpExtInst (GLSL.std.450 Fma)
    EXPECT_STREQ(spirv::opcode_name(12), "OpExtInst");
}

TEST(Spirv, EndpgmStopsTranslation) {
    // s_endpgm 0 followed by an add that must not be translated.
    const uint32_t code[] = {0xBF810000u, 0x06040301u};
    spirv::ShaderInterface iface;
    auto out = spirv::emit_fragment(std::span<const uint32_t>(code, 2), iface);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->stats.endpgm, 1u);
    EXPECT_EQ(out->stats.translated, 1u); // only the s_endpgm itself
    EXPECT_EQ(out->stats.decoded, 1u);
    auto info = spirv::inspect(out->words);
    ASSERT_TRUE(info.has_value());
    EXPECT_TRUE(info->body_opcodes.empty());
}

TEST(Spirv, UnsupportedOpsAreCounted) {
    // v_fmac_f32 v2, v0, v1 (op 0x2B, not translatable)
    const uint32_t fmac = (0x2Bu << 25) | (2u << 17) | (1u << 9) | 0x101u;
    const uint32_t code[] = {fmac};
    spirv::ShaderInterface iface;
    auto out = spirv::emit_fragment(std::span<const uint32_t>(code, 1), iface);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->stats.decoded, 1u);
    EXPECT_EQ(out->stats.translated, 0u);
    EXPECT_EQ(out->stats.skipped, 1u);
}

TEST(Spirv, PartialExportSkipped) {
    // EN=0x8 partial export is deferred, not translated.
    const uint32_t code[] = {0xF8000808u, 0x01000000u};
    spirv::ShaderInterface iface;
    auto out = spirv::emit_fragment(std::span<const uint32_t>(code, 2), iface);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->stats.skipped, 1u);
}

TEST(Spirv, MultipleOutputLocations) {
    // Two exports: MRT0 (v0..v3) and MRT1 (v4..v7) with two input locations.
    // TARGET sits at [9:4]: MRT1 = 1 -> byte0 = EN(0xF) | (1 << 4) = 0x1F.
    const uint32_t code[] = {0xF800000Fu, 0x03020100u,   // exp mrt0 v0-v3
                             0xF800001Fu, 0x07060504u};  // exp mrt1 v4-v7
    spirv::ShaderInterface iface;
    iface.input_locations = 2;   // seeds v0..v7
    iface.output_locations = 2;
    auto out = spirv::emit_fragment(std::span<const uint32_t>(code, 4), iface);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->stats.translated, 2u);
    auto info = spirv::inspect(out->words);
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->input_locations.size(), 2u);
    ASSERT_EQ(info->output_locations.size(), 2u);
    EXPECT_EQ(info->output_locations[1], 1u);
}

TEST(Spirv, EmptyInputRejected) {
    spirv::ShaderInterface iface;
    auto out = spirv::emit_fragment(std::span<const uint32_t>(), iface);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Status::ParseError);
}

TEST(Spirv, InspectRejectsBadMagic) {
    const uint32_t words[] = {0xDEADBEEFu, 0, 0, 0, 0};
    auto info = spirv::inspect(std::span<const uint32_t>(words, 5));
    ASSERT_FALSE(info.has_value());
    EXPECT_EQ(info.error().code, Status::ParseError);
}

// ---------- Vulkan runtime probe ----------
TEST(VulkanProbe, ReportsMissingLoaderAsExpectedError) {
    auto info = vulkan::probe_runtime();
    if (!info.has_value()) {
        EXPECT_EQ(info.error().code, Status::VulkanError);
        EXPECT_FALSE(info.error().message.empty());
        GTEST_SKIP() << "no Vulkan runtime on this host: " << info.error().message;
    }
    EXPECT_FALSE(info->to_json().empty());
    for (const auto& d : info->devices) {
        EXPECT_FALSE(d.device_name.empty());
        EXPECT_GE(d.api_version, (1u << 22)); // at least Vulkan 1.0
    }
}
