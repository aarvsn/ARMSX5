# Testing ARMSX5

## Native suites (GoogleTest, host)

| Suite | Tests | Coverage |
|---|---|---|
| armsx5_tests_core_memory | 17 | Config store; guest memory (map/unmap/protect/translate/permissions/overlap/bump allocator/exec tracking/address-space limits) |
| armsx5_tests_loader | 15 | Valid ELF64 images (ET_EXEC + ET_DYN), malformed battery (magic, truncation, machine, class, bounds, SELF detection, entry checks) |
| armsx5_tests_cpu | 40 | Decoder, integer interpreter semantics (ALU/flags/shifts/stack/calls/memory), register partial-write rules, **SSE subset** (moves incl. upper-lane preservation + alignment faults, scalar/packed arithmetic, CMP predicates, COMIS flags, conversions with integer-indefinite, packed int ops, PSHUFD, VEX/AVX rejection), ARM64 encoder encodings, JIT translation/cache/fallback |
| armsx5_tests_system | 12 | Kernel syscalls (exit/write/mmap/munmap/EBADF/ENOSYS/yield), VFS (MemFS, host-dir traversal protection), HLE registry |
| armsx5_tests_gpu | 16 | PM4 decode (valid/multi/unknown/type rejection/truncation), resource registry, RDNA2 format decode, Vulkan probe (skips without a loader) |
| armsx5_tests_integration | 5 | Original homebrew ELFs end-to-end (hello/sum/callret), session cleanup, malformed-input rejection |

Total: **105 passing** + 1 environment-skip (Vulkan probe on hosts without a
loader/ICD — the skip is loud, not silent).

Run:

```sh
ctest --test-dir build/host --output-on-failure
```

## Homebrew test executables (original, legally clean)

`tools/test_elfs/*.S` are freestanding x86-64 programs assembled by the
build:

- `hello.S` — write(1, "Hello from ARMSX5 guest\n") + exit(42)
- `sum.S` — loop sum 1..100 → exit(5050) (ALU, flags, backward branch)
- `callret.S` — CALL/RET/LEA round trip → exit(12)

They call the emulated kernel through our documented test ABI (raw
FreeBSD/amd64 `syscall` instructions). These binaries are the only
executables CI and the on-device Diagnostics screen ever run.

## Android

- `./gradlew testDebugUnitTest` — pure-Kotlin unit tests (no device).
- Instrumentation tests are not yet provided; the Diagnostics screen
  executes the packaged `hello.elf` through the real pipeline on-device.

## SSE testing notes

The SSE tests hand-encode instruction bytes with a `modrm()` helper. Two
rules learned while writing them (keep them for future tests):

1. RIP-relative displacement is measured from the END of the instruction
   (`next_rip = base + offset + length`), so a table at `base+0x100` after a
   7-byte instruction needs disp `0x100 - 7`.
2. Memory tables must lay out 4 consecutive float lanes per 16 bytes —
   interleaving 4 zero padding bytes per float changes which lane movaps
   loads.

## Differential testing policy

The interpreter is the correctness reference. JIT blocks must match its
results; the comparison harness runs automatically on aarch64 hosts and the
encoder/translation tests run everywhere. A JIT/interpreter divergence is a
release blocker.

## Rules

- Never disable a failing test to make CI green; fix the code or document
  the environment skip explicitly (GTEST_SKIP with a reason).
- Every bug fix adds a regression test where practical (the SSE tests
  exist exactly because of the MOVD-direction, SS/PS-double-overlap, and
  SQRT-source bugs found while writing them).
