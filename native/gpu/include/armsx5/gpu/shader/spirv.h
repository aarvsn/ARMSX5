#pragma once
// Minimal SPIR-V emitter for the RDNA2 shader translator.
//
// Emits a fragment-stage module from a decoded RDNA2 program slice:
//  - VGPR v[4*i .. 4*i+3] are seeded from generic input variables (one vec4
//    per location), the RDNA2 pipeline register model.
//  - EXP exports with target 0-7 (MRT0-7) store a vec4 built from four VGPRs
//    to the matching output variable.
//  - SGPRs are exposed as a push-constant block (u32 array) so host-side
//    descriptor constants can be bound later; S_MOVK_I32 materializes
//    immediates instead.
//  - Straight-line SSA tracking: each VGPR/SGPR holds its current SPIR-V id
//    and type; float/integer ops insert OpBitcast when the tracked type
//    differs (no per-lane SSA pruning yet).
//
// The binary layout is SPIR-V 1.0 (magic 0x07230203, version 1.0). Opcode
// numbers were verified against the Khronos SPIRV-Headers core grammar and
// the GLSL.std.450 extended-instruction set (FMin 37 / UMin 38 / SMin 39 /
// FMax 40 / UMax 41 / SMax 42 / Fma 50).
#include "armsx5/core/result.h"

#include <expected>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace armsx5::gpu::spirv {

constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr uint32_t kSpirvVersion10 = (1u << 16) | 0u;  // major 1, minor 0

struct ShaderInterface {
    uint32_t input_locations = 1;   // vec4<f32> inputs (4 VGPRs each)
    uint32_t output_locations = 1;  // vec4<f32> outputs (MRT0..N-1)
    uint32_t sgpr_push_count = 16;  // SGPR window exposed as push constants
};

struct EmitStats {
    uint32_t decoded = 0;      // instructions successfully decoded
    uint32_t translated = 0;   // instructions emitted to SPIR-V
    uint32_t skipped = 0;      // decoded but not translatable
    uint32_t decode_errors = 0; // reserved/truncated words
    uint32_t endpgm = 0;       // S_ENDPGM encountered (program tail)
};

struct EmitResult {
    std::vector<uint32_t> words;  // SPIR-V binary
    uint32_t id_bound = 0;
    EmitStats stats;
};

// Translates a raw RDNA2 code slice into a SPIR-V fragment module. Words
// after S_ENDPGM are ignored. Returns ParseError if the input is empty and
// InvalidArgument if the interface is inconsistent.
std::expected<EmitResult, Error> emit_fragment(std::span<const uint32_t> code,
                                               const ShaderInterface& iface);

// Structural summary of a SPIR-V module, produced by a light walker used by
// unit tests and self-checks. It validates the header, instruction word
// counts and id-bound compliance for the subset the emitter produces.
struct SpirvInfo {
    uint8_t version_major = 0;
    uint8_t version_minor = 0;
    uint32_t generator = 0;
    uint32_t id_bound = 0;
    uint32_t schema = 0;
    uint32_t instruction_count = 0;
    uint32_t capability_count = 0;
    uint32_t function_count = 0;
    uint32_t label_count = 0;
    uint32_t return_count = 0;
    bool has_glsl_ext = false;
    bool entry_point_fragment = false;
    bool memory_model_logical_glsl450 = false;
    bool push_constant_block = false;
    std::vector<uint32_t> input_locations;
    std::vector<uint32_t> output_locations;
    std::vector<uint16_t> body_opcodes;  // arithmetic/logic ops inside main
    std::vector<std::string> errors;     // structural problems found
};

std::expected<SpirvInfo, Error> inspect(std::span<const uint32_t> words);

// Name of the SPIR-V core opcodes the emitter produces ("OpIAdd", ...);
// "?" for anything else.
const char* opcode_name(uint16_t op);

} // namespace armsx5::gpu::spirv
