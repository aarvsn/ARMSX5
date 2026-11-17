# Changelog

## Unreleased

### CI fixes

- `ci.yml` now has the sub-workflows declare `workflow_call`, so the
  aggregate workflow no longer fails before any job is scheduled
  ("workflow_call event trigger is not found"); `native-tests` and
  `android-build` run as reusable workflows and keep their manual
  `workflow_dispatch` trigger.
- Native and Android jobs fetch the vendored dependencies
  (`scripts/setup-deps.sh`) before configuring, so the Vulkan headers that
  `native/gpu` needs are present (previously the build failed on
  `vulkan/vulkan.h: No such file or directory`).
- Android SDK setup passes `packages: platform-tools`; the action's default
  `tools` package no longer exists in the SDK repository and aborted setup
  with `Failed to find package 'tools'`. The platform/build-tools/NDK/CMake
  install now requests `platforms;android-34` and `build-tools;34.0.0`
  explicitly.
- `android/gradle.properties` no longer pins `org.gradle.java.home` to a
  developer-local path; Gradle uses the JDK provided by the environment.
- `android/gradlew` and `scripts/*.sh` are committed executable
  (mode 100755), so `./gradlew` and the scripts run on a fresh checkout.

## 0.4.0 — 2026-11-17

### GPU

- **RDNA2 decoder rewritten on hardware-verified layouts.** The previous
  classifier decoded an *invented* encoding (VOP2 prefix 0x09, VOP1 0x13,
  9-bit dst at [8:0], EXP target at [15:6]) that does not exist on GFX10.
  Every family now matches LLVM's MC encodings and tablegen field
  definitions: VOP1/VOP2/VOPC share bit31=0 (op6 = [30:25]), VOP2 vdst moved
  to [24:17] with a 9-bit src0 (bit8 = VGPR select), VOP3 e64 carries a
  10-bit opcode at [25:16] with 9-bit srcs, SOPK/SOP1/SOPC/SOPP split on
  bits[28:23], EXP uses EN=[3:0]/TARGET=[9:4] with srcs in the second dword.
  The old tests encoded the wrong bits; they now use literal byte sequences
  from LLVM's gfx10_asm_*.s MC tests.
- **Opcode semantic table** for ~90 GFX10 opcodes (mnemonic, class,
  float/int, translatable flag) with the GFX10 renumbering applied
  (S_MOV_B32 = 3, S_AND_B32 = 14, V_MOV_B32 = VOP1 op 1, V_ADD_F32 = VOP2
  op 3, V_READLANE = VOP3 0x360, S_ENDPGM = SOPP op 1) and the scalar-source
  literal table (SGPR/vcc/m0/exec/null, int consts 128-208, float consts
  240-247, literal 255).
- **Minimal SPIR-V emitter** (`native/gpu/shader/spirv.cpp`): straight-line
  RDNA2 -> SPIR-V 1.0 fragment module. VGPRs v[4i..4i+3] seed from vec4
  input variables (per location), EXP MRT0-7 (full mask) stores vec4 to
  output variables, SGPRs read through a push-constant block, S_MOVK_I32
  materializes sign-extended immediates, S_ENDPGM ends the program. ~35
  opcodes translate end-to-end with type tracking and automatic OpBitcast
  between f32/u32/i32 (V_ADD/SUB/SUBREV/MUL/LEGACY, V_ADD_NC/SUB_NC/
  SUBREV_NC, V_AND/OR/XOR/XNOR, V_LSHLREV/LSHRREV/ASHRREV, V_MIN/MAX
  F32/I32/U32, V_CVT_F32_I32/U32, V_CVT_U32/I32_F32, V_TRUNC/CEIL/RNDNE/
  FLOOR, V_RCP/SQRT/NOT, V_MOV_B32, V_NOP, V_FMA_F32, V_MIN3/MAX3_F32 e64,
  S_MOV_B32/S_NOT_B32/S_ADD/SUB/AND/OR/XOR/MUL_U32/S_LSHL/LSHR/ASHR,
  S_MOVK_I32, S_ENDPGM, EXP). All SPIR-V core + GLSL.std.450 opcode numbers
  verified against Khronos SPIRV-Headers.
- `inspect()` structural walker validates the emitted module (header,
  version, word counts, id bound, decorations, entry point, memory model)
  and is used by the unit tests as a self-check.
- New GPU tests: 27 additions (suite total 43): verified decode tests for
  every family, literal/two-word handling, EXP target/en/done fields,
  memory-family prefix classification, semantic-table lookups, scalar
  operand decoding, and 10 SPIR-V emitter/inspect tests. Project total now
  158 tests (157 passed + 1 environment-skip) across 6 suites.

### GPU fixes found by the verification work

- SOPK family detection: `bits[28:23]` encode `0x20 + op` (bit28 fixed),
  not a sub-opcode — SOPK op 29-31 would otherwise alias SOP1/SOPC/SOPP.
- VOP2 e32 `src1` is VGPR-only on GFX10 (8 bits); the emitter no longer
  routes it through the scalar-source table.
- VOP2-class-in-VOP3 opcode range is 0x100-0x13F (V_FMA_F32 0x14B and
  friends are VOP3-only ops, not VOP2 forms).
- EXP `done` flag is bit 11 of word0 (second byte bit3), not bit 8.

## 0.3.0 — 2026-11-16

### CPU

- **AVX (VEX) subset implemented and tested**: both VEX encodings decode
  (0xC5 two-byte, 0xC4 three-byte) with RXB.mmmmm opcode maps, W, L and pp
  fields and complement-decoded vvvv (registers xmm0-15/ymm0-15 reachable).
  The full SSE operation set now runs in its non-destructive three-operand
  AVX forms at both 128-bit and 256-bit vector lengths: VMOVUPS/VMOVAPS/
  VMOVDQA/VMOVDQU/VMOVSS/VMOVSD/VMOVD/VMOVQ (+stores), VADD/VSUB/VMUL/VDIV/
  VSQRT (SS/SD/PS/PD), VAND/VANDN/VOR/VXORPS(PD), VCMP* with the eight IEEE
  predicates, VUCOMISS/VCOMISS/VUCOMISD/VCOMISD, VCVTSI2SS/VCVTSI2SD/
  VCVTTSS2SI/VCVTTSD2SI/VCVTSS2SI/VCVTSD2SI, VCVTDQ2PS/VCVTPS2DQ/
  VCVTTPS2DQ, VPADD*/VPSUB*/VPMULLW/VPMULUDQ, VPSHUFD.
- New `CpuState.ymm_hi[16]` state carries YMM[255:128]; `Vec256` and shared
  lane helpers (`src/vec_ops.h`) serve both interpreters. 24 new CPU tests
  (suite total 64; project total 131) plus a fourth homebrew ELF
  (`simdavx.elf`) running a full VEX compute->store->GPR-reduce->exit
  pipeline (both C4 and C5 encodings, ymm8-15 via VEX.R/X/B, exit 576).
- VEX semantics were verified against real AVX hardware before
  implementation (`scripts/verify_avx.c`): VEX.128 zeroing of YMM upper
  halves (including VMOVSS/VMOVSD/VCVTSI2SS destinations), scalar
  upper-lane propagation from SRC1, packed sqrt/convert/shuffle ignoring
  vvvv, and VPSHUFD half-independent shuffling. The rules are documented in
  docs/architecture/cpu.md.

### Loader

- PT_LOAD mapping now floor-aligns segment vaddrs to the page base (ELF
  only requires p_offset ≡ p_vaddr mod page size), matching real program
  loaders. Found by the new AVX test ELF: ld-placed .data segments with
  mid-page vaddrs previously failed to map.

## 0.2.0 — 2026-11-16

### CPU

- **SSE subset implemented and tested**: MOVSS/MOVSD/MOVUPS/MOVAPS/MOVDQA/
  MOVDQU/MOVD/MOVQ (with hardware-accurate upper-lane preservation and
  16-byte alignment enforcement for aligned forms), ADD/SUB/MUL/DIV/SQRT in
  SS/PS/SD/PD forms, AND/NAND/OR/XORPS, CMPSS/CMPPS/CMPSD/CMPPD with the
  eight IEEE predicates, COMISS/UCOMISS/COMISD/UCOMISD flag semantics,
  CVTSI2SS/CVTSI2SD/CVTTSS2SI/CVTSS2SI/CVTDQ2PS/CVTPS2DQ/CVTTPS2DQ with
  integer-indefinite handling, PADD*/PSUB*/PMULLW/PMULUDQ, PSHUFD.
- AVX (VEX-encoded) instructions decode as Unsupported with a clear message.
- 11 new CPU tests (suite total 40; project total 105).

### Fixes found by the new SSE tests

- MOVD/MOVQ operand direction (66 0F 6E vs 66/F3 0F 7E vs 66 0F D6) is now
  driven by explicit SseForm variants instead of size heuristics.
- SS/PS arithmetic no longer also applies the packed-double path (which
  corrupted lanes with reinterpreted bit patterns).
- SQRTSS/SQRTPS read the source operand (unary), not the destination.

## 0.1.0 — 2026-11-16 (initial engineering milestone)

### Native core (C++23)

- Guest memory manager: map/unmap/protect with bounds + permission
  enforcement, bump/reservation allocators, exec-page tracking.
- ELF64 loader: full validation battery, ET_EXEC/ET_DYN mapping with
  slide, SELF detection (documented Unsupported), file-inspection API.
- x86-64 decoder + interpreter: integer subset with exact flag semantics,
  div-fault handling, partial-register rules; unsupported instructions
  stop with diagnostics instead of mis-executing.
- ARM64 encoder (unit-tested encodings) and JIT: block cache, native
  fast paths with helper fallbacks, exec-page invalidation, interpreter
  fallback for untranslatable instructions.
- Kernel: FreeBSD/amd64 syscall baseline (exit/write/mmap/munmap/
  mprotect/sched_yield), CF+errno error convention, unsupported-call
  accounting; VFS with path-traversal protection; HLE module registry.
- Process runtime: end-to-end session pipeline with RunResult reporting.
- GPU: PM4 Type-3 decoder, resource registry with residency, RDNA2 format
  classifier, Vulkan runtime probe (dlopen, no link dependency).
- Audio: lock-free ring buffer, null sink, AAudio Android backend.
- Input: Android-keycode mapping, deadzone handling, state aggregator.
- Platform: executable-memory arena (Android memfd dual-map W^X, desktop
  RWX); GoogleTest suites + 3 original homebrew ELFs running end-to-end.

### Android frontend (Kotlin, Compose, Material 3)

- Home / Library / Game details / Settings / Diagnostics / About screens.
- SAF folder import + validation flow; on-device execution of packaged
  homebrew test ELFs through the native pipeline.
- Device capability reporting (SoC, ABIs, Vulkan probe via JNI).
- GameController event forwarding into the native input aggregator.

### Tooling / CI / docs

- armsx5-runner CLI; scripts/build_host.sh, scripts/sync_assets.sh,
  scripts/setup-deps.sh.
- GitHub Actions: android-build, native-tests, ci, release.
- Research (upstream audit, reuse matrix, license audit, Android
  portability, verified facts, SELF blocker), architecture, development,
  compatibility docs.
