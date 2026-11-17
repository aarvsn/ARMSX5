# GPU Architecture

## What exists (implemented and tested)

- **PM4 command decoding** (`native/gpu/pm4`): Type-3 packet parsing with
  the documented header layout (type [31:30], count [29:16], opcode
  [15:8]); a named-opcode table for the common radeonsi/GNM family opcodes
  (DRAW_INDEX, INDEX_TYPE, NUM_INSTANCES, NOP, SET_BASE, CLEAR_STATE,
  DRAW_INDEX_AUTO, EVENT_WRITE, SET_CONTEXT_REG, SET_SH_REG,
  SET_SH_REG_OFFSET). Unknown opcodes are counted and preserved raw — never
  interpreted.
- **Resource registry** (`native/gpu/resource.cpp`): guest buffer/image
  registration with overlap rejection and a residency model
  (Resident/Evicted) for later VRAM management.
- **RDNA2 decoder + opcode semantic table** (`native/gpu/shader/rdna2.cpp`):
  hardware-verified GFX10/10.3 format classification and field extraction for
  SOP1/SOP2/SOPK/SOPC/SOPP/VOP1/VOP2/VOP3(e64)/VOPC/EXP, plus a curated
  semantic table (mnemonic, operand class, float/int, translatable flag) for
  ~90 opcodes. Every field layout and opcode number was cross-checked against
  LLVM's MC test encodings (`llvm/test/MC/AMDGPU/gfx10_asm_*.s`, literal
  hardware-validated byte sequences) and LLVM tablegen field definitions
  (`VOPInstructions.td`, `SOPInstructions.td`). Memory families
  (DS/FLAT/MUBUF/MTBUF/MIMG/SMEM) are classified by prefix only; VOPC decodes
  opcode + src0 but has no semantic table yet.
- **Minimal SPIR-V emitter** (`native/gpu/shader/spirv.cpp`): translates a
  straight-line RDNA2 program slice into a SPIR-V 1.0 fragment module. VGPR
  v[4i..4i+3] are seeded from vec4 input variables, MRT0-7 exports store to
  vec4 output variables, SGPRs are exposed as a push-constant block, and
  ~35 opcodes translate end-to-end (f32/u32 arithmetic, bit logic, shifts,
  min/max via GLSL.std.450, conversions, FMA/MIN3, V_MOV/V_NOP/S_MOVK/
  S_MOV_B32/S_*_U32/S_AND/S_OR/S_XOR/S_MUL/S_ENDPGM, full-mask EXP). Type
  tracking inserts OpBitcast when a value's producer type differs from the
  consumer's needs. A structural `inspect()` walker validates the emitted
  binary (header, word counts, id bounds) and is unit-tested.
- **Vulkan runtime probe** (`native/gpu/vulkan/device_info.cpp`): dlopen
  loader, instance creation, physical-device enumeration with API version,
  vendor/device IDs, driver version and extension list; JSON reporting.

## What does not exist yet (honest)

- **Guest GPU command processor execution**: packets decode; nothing
  executes them against a rendering pipeline.
- **Deeper shader translation**: control flow (branches/loops), SMEM/DS/
  FLAT/MUBUF/MIMG memory instructions, VOPC compares feeding EXEC/VCC,
  compressed exports (COMPR), partial exports (EN != 0xF), and descriptor
  mapping (S_LOAD + samplers/buffers) are all unimplemented — the emitter
  covers straight-line ALU + export only. This remains the main Phase-5/7
  engineering mountain.
- **Swapchain/presentation**: the Android backend compiles against the
  NDK's Vulkan headers but no surface/swapchain wiring exists yet.
- **Shader/pipeline caches**: designed (resource registry + capability
  probe exist) but not implemented.

## Non-negotiable rules

- Successful Vulkan instance/device initialization is a *capability*, not
  GPU emulation. ARMSX5 must never claim RDNA2 emulation because "Vulkan
  initialized".
- PS5-vs-PS4 differences (GFX10.3.x opcodes, shader programmable state) mean
  shadPS4's recompiler is a reference, not a drop-in.

## Verified GFX10/10.3 (RDNA2) encoding layout

Family detection on the first 32-bit word (`w0`):

| Condition | Family | Notes |
|---|---|---|
| `w0[31] == 0` | VOP1/VOP2/VOPC | `op6 = w0[30:25]`: 0-61 VOP2, 62 VOPC, 63 VOP1 |
| `w0[31:26] == 53` | VOP3 (8 bytes) | e64 forms |
| `w0[31:26] == 54/55/56/58/60/61` | DS/FLAT/MUBUF/MTBUF/MIMG/SMEM | classified only |
| `w0[31:26] == 62` | EXP (8 bytes) | |
| `w0[31:28] == 0b1011` | SOP family | `v = w0[28:23]`: 61 SOP1, 62 SOPC, 63 SOPP, else SOPK (`op = v - 32`) |
| `w0[31:26] in 32..43` | SOP2 | `op = w0[29:23]` |

Field maps (LLVM tablegen `Inst{...}` assignments, confirmed by MC encodings):

- **VOP2 e32** (`VOP2e`): `op = [30:25]` (6 bits), `vdst = [24:17]`,
  `src1 = [16:9]` (8 bits, VGPR-only on GFX10 — scalar sources force e64),
  `src0 = [8:0]` (9 bits: bit8 = VGPR select, `[7:0]` = scalar table value).
- **VOP1 e32** (`VOP1e`): prefix `[31:25] = 0x3F`, `vdst = [24:17]`,
  `op = [16:9]` (8 bits), `src0 = [8:0]`.
- **VOP3 e64** (`VOP3a_gfx10`): word0 `op = [25:16]` (10 bits),
  `clamp = [15]`, `vdst = [7:0]`; word1 `src0 = [8:0]`, `src1 = [17:9]`,
  `src2 = [26:18]` (each 9 bits with VGPR-select bit8), `omod = [28:27]`.
  VOP2-class ops occupy `0x100 + VOP2opcode` (0x100-0x13F); VOP3-only ops
  start at 0x140 (V_FMA_F32 = 0x14B); VOPC-class at 0x200+; VOP1-class
  (V_READLANE = 0x360) at 0x300+.
- **EXP**: word0 `en = [3:0]`, `target = [9:4]` (6 bits: 0-7 MRT0-7,
  8 MRTZ, 9 NULL, 12 POS0, 20 PRIM, 32-63 PARAM0-31), `compr = [10]`,
  `done = [11]`, `vm = [12]`; word1 `src0 = [7:0]`, `src1 = [15:8]`,
  `src2 = [23:16]`, `src3 = [31:24]` (raw VGPR indices).
- **SOP1**: prefix `[31:23] = 0x17D`, `sdst = [22:16]`, `op = [15:8]`
  (GFX10 renumbered: S_MOV_B32 = 3), `src0 = [7:0]`.
- **SOP2**: `[31:30] = 0b10`, `op = [29:23]`, `sdst = [22:16]`,
  `src1 = [15:8]`, `src0 = [7:0]` (GFX10 renumbered: S_AND_B32 = 14).
- **SOPK**: `[31:28] = 0b1011`, `op = [27:23]`, `sdst = [22:16]`,
  `simm16 = [15:0]` (S_MOVK_I32 = 0, S_ADDK_I32 = 15, S_CALL_B64 = 22).
- **SOPP**: prefix `[31:23] = 0x17F`, `op = [22:16]`, `simm16 = [15:0]`
  (S_NOP = 0, S_ENDPGM = 1, S_BRANCH = 2).

Scalar source table (8-bit literal fields): 0-101 SGPR, 106/107 vcc lo/hi,
124 m0, 125 null, 126/127 exec lo/hi, 128-192 unsigned constants 0-64,
193-208 negative constants -1..-16, 240-247 float constants
(0.5, -0.5, 1.0, -1.0, 2.0, -2.0, 4.0, -4.0), 255 inline literal (extra
dword follows the instruction).
