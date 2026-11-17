# ARMSX5 Architecture Overview

ARMSX5 is an Android (ARM64-v8a) emulator targeting PlayStation 5 software,
structured as a portable C++23 core with a Kotlin/Compose frontend.

```
android/                        Kotlin + Jetpack Compose (Material 3) frontend
  app/src/main/cpp/             JNI bridge -> libarmsx5_jni.so
native/
  core/      logging, status codes, configuration
  memory/    guest address space (map/protect/translate, exec tracking)
  loader/    ELF64 validation + image mapping (SELF -> documented Unsupported)
  cpu/       x86-64 decoder, interpreter (reference; integer + SSE subset),
             IR, ARM64 encoder, JIT
  system/    syscall kernel (FreeBSD-style), VFS, HLE registry, process runtime
  gpu/       PM4 decoder, resource registry, RDNA2 decode scaffold, Vulkan probe
  audio/     ring buffer + AAudio backend (Android), null backend (tests)
  input/     controller mapping/aggregation (Android keycodes -> guest buttons)
  platform/  executable-memory arena (Android W^X dual-map), host utilities
tests/       GoogleTest suites (105 tests) + original homebrew ELFs (integration)
tools/       armsx5-runner CLI, homebrew ELF sources
```

## Data flow for a guest session

1. Frontend imports a file (SAF -> cache) and calls
   `nativeValidateExecutable` (JNI) -> `ElfLoader::inspect_file`.
2. `ProcessRuntime::run` maps the image into the guest address space,
   allocates a guest stack, and primes RIP/RSP.
3. The interpreter fetches, decodes, and executes instructions against the
   memory manager; syscalls go to the Kernel (FreeBSD/amd64 conventions),
   and write(1/2) output is captured for the frontend.
4. Optional JIT acceleration (aarch64 hosts) translates straight-line blocks
   to AArch64 code with helper fallbacks; unsupported instructions end the
   block and fall back to the interpreter.

## Non-negotiable design rules

- Correctness before performance; the interpreter is the reference engine.
- Guest memory access always goes through bounds/permission-checked
  translation — malformed executables cannot corrupt host memory.
- Unsupported instructions/syscalls fail loudly (Status::Unsupported /
  ENOSYS) instead of fabricating success.
- Android specifics (W^X exec memory, AAudio, JNI) stay behind interfaces;
  the portable core builds and tests on desktop Linux.
- Every subsystem ships with unit tests; integration tests run original
  homebrew ELFs end-to-end.

## Status (honest)

Implemented and tested: loader, memory, interpreter (integer + SSE subset),
kernel test ABI, PM4/RDNA2 decode layers, Vulkan probing, audio/input
frameworks, Android frontend with on-device homebrew execution.
Scaffolded, not claiming parity: ARM64 JIT (translation tested; execution
requires aarch64 hardware), RDNA2-to-SPIR-V translation, PS5 SELF loading
(legal blocker), PS5 kernel/HLE interfaces, guest GPU rendering.

See docs/development/status-report.md for the phase-by-phase report.
