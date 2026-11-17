# CPU Architecture

## Components

- `x86/decoder.{h,cpp}` — long-mode decoder for the implemented integer
  subset **plus an SSE subset and an AVX subset**. Integer: ALU block
  (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP in all addressing forms), MOV variants,
  LEA, PUSH/POP, CALL/RET, Jcc/JMP, SETcc/CMOVcc, MOVZX/MOVSX/MOVSD, shifts
  (ROL/ROR/SHL/SHR/SAL/SAR), MUL/IMUL (all forms), DIV/IDIV, NEG/NOT/INC/DEC,
  XCHG, SYSCALL, NOPs, CDQE/CDQ/CQO. SIB, RIP-relative addressing,
  66/F2/F3/REX prefixes handled. SSE: MOVSS/MOVSD/MOVUPS/MOVAPS/MOVDQA/
  MOVDQU/MOVD/MOVQ, ADD/SUB/MUL/DIV/SQRT (SS/PS/SD/PD), AND/ANDN/OR/XORPS(PD),
  CMPSS/CMPPS/CMPSD/CMPPD (8 predicates), COMISS/UCOMISS/COMISD/UCOMISD,
  CVTSI2SS/CVTSI2SD/CVTTSS2SI/CVTSS2SI/CVTTSD2SI/CVTSD2SI, CVTDQ2PS/
  CVTPS2DQ/CVTTPS2DQ, PADD*/PSUB*/PMULLW/PMULUDQ, PSHUFD. AVX (VEX):
  the VEX.128/VEX.256 mirrors of the same operations, decoded from both
  VEX encodings (0xC5 two-byte, 0xC4 three-byte) with RXB.mmmmm, W, L, pp
  and the complement-decoded vvvv register; string ops, bit tests, 0F38/
  0F3A opcode maps and privileged opcodes decode as `Unsupported` — the
  runtime reports them instead of mis-executing.
- `interpreter.{h,cpp}` + `interpreter_sse.cpp` + `interpreter_avx.cpp` —
  reference engine. Integer: exact flag semantics (CF/PF/AF/ZF/SF/OF) with
  sign-based overflow tests correct for ADC/SBB; #DE on division faults;
  partial-register writes follow x86 rules. SSE/AVX: IEEE-754 with the
  default round-to-nearest-even mode; MXCSR exception flags are not modeled
  (documented); integer-indefinite (0x80000000...) returned for NaN/
  out-of-range conversions like real hardware; aligned moves enforce
  natural alignment (16 bytes at VEX.128, 32 bytes at VEX.256; hardware #GP
  becomes a guest fault).
- `vec_ops.h` — shared lane accessors (dword/qword/float/double lanes over
  128- and 256-bit views) used by both interpreters.
- AVX architectural state: `CpuState.xmm[16]` holds the low 128 bits of
  YMM0-15 and `CpuState.ymm_hi[16]` the upper halves. The VEX semantics
  implemented here were verified against real AVX hardware
  (`scripts/verify_avx.c`):
  - VEX.128 instructions zero YMM[255:128] of the destination — including
    VMOVSS/VMOVSD (register and memory forms) and VCVTSI2SS, unlike legacy
    SSE which leaves the upper half untouched.
  - Three-operand layout: dst = modrm.reg, src1 = vvvv, src2 = modrm.rm.
    Scalar binary ops (VADDSS et al): DEST[31:0] = SRC1 op SRC2 and
    DEST[127:32] = SRC1[127:32]. Scalar unary ops (VSQRTSS/VSQRTSD):
    DEST[31:0] = sqrt(SRC2), DEST[127:32] = SRC1[127:32]. Packed ops:
    every lane = SRC1 op SRC2; packed sqrt/convert/shuffle do not read
    vvvv (hardware #UDs when it is not 1111b; this interpreter is lenient).
  - VMOVD/VMOVQ zero every bit above the moved lane; VMOVSS/VMOVSD
    memory-source form zeroes DEST[127:32] (legacy MOVSS preserves it).
  - VPSHUFD shuffles the two 128-bit halves independently at VEX.256.
- `ir.h` — linear block IR for the JIT.
- `arm64/encoder.{h,cpp}` — dependency-free AArch64 emitter; every encoding
  is unit-tested against words derived from the ARM ARM field layout.
- `jit.{h,cpp}` — block cache keyed by guest RIP. Fast paths: MOV/IMM/LEA
  as native AArch64 code. ALU, memory, flags, and control flow go through
  host helper calls (correctness-first v1). Blocks end before any
  unsupported instruction, so the interpreter always handles what the JIT
  cannot translate (VEX-encoded instructions take this path today). Guest
  stores into executable pages trigger a conservative cache flush.

## Executable memory

- Desktop hosts: RWX anonymous mapping (development only).
- Android: `memfd_create` + dual mapping (RW writer view / RX executor
  view) — see docs/research/android-portability.md.

## Differential testing policy

The JIT must produce results identical to the interpreter. The harness and
golden tests are wired into `tests/cpu`; execution-comparison tests run
only on aarch64 hosts (the x86-64 CI validates translation, encodings, and
cache behavior; on-device verification is an open item tracked in the
status report).

## Known limits (documented, not hidden)

- AVX subset only: no VEX 0F38/0F3A maps, no gather/scatter, no FMA, no
  per-element rounding modes, no AVX-512. vvvv-unused encodings are
  accepted leniently where hardware would #UD.
- No string ops, bit tests, privileged instructions.
- No ring-3 page-fault delivery, no interrupt model: guest faults surface
  as `Status::GuestFault` with the faulting RIP.
- No guest multi-threading: `thr_new` returns ENOSYS and is counted.
- MXCSR FP exception flags not modeled; rounding is the host default
  (nearest-even).
