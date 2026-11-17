# ARMSX5

**Android Runtime and Machine-System eXecution for PlayStation 5**

ARMSX5 is an open-source (GPL-2.0-only) research project whose long-term
objective is to run legally obtained commercial PlayStation 5 software on
Android (ARM64-v8a), written in Kotlin + C++23 with Vulkan graphics.

## Current development status (honest)

ARMSX5 is a real, working foundation, **not** a PS5 emulator yet:

- The native core (C++23) builds on desktop Linux and on Android (NDK),
  with **105 passing GoogleTest tests** across six suites.
- The complete pipeline works end-to-end for plain ELF64 x86-64 test
  programs: validate → map → execute (interpreter) → kernel syscalls →
  captured output. The Android app ships and runs original homebrew test
  executables on-device through this pipeline (Diagnostics screen).
- CPU: integer subset **plus an SSE subset** (scalar/packed float moves and
  arithmetic, comparisons, conversions, COMIS*/UCOMIS*, packed integer
  add/sub/mul, PSHUFD) with exact flag semantics; AVX (VEX) is reported as
  Unsupported.
- The ARM64 JIT translates verified integer blocks (encoding unit tests);
  native execution requires aarch64 hardware and remains experimental.
- PM4 command-buffer decoding, GPU resource tracking, an RDNA2 format
  classifier, and Vulkan runtime probing are implemented; **guest GPU
  rendering and RDNA2→SPIR-V translation are not implemented yet**.
- PS5 SELF executables are detected and **reported as unsupported** —
  loading them is legally blocked for this project today
  (docs/research/self-loading.md). Commercial PS5 games do not run.

## Supported platforms

- Host development: Linux x86_64 (native tests, armsx5-runner).
- Target: Android 12+ (API 31+), arm64-v8a. No root access, no dangerous
  permissions.

## Build

Prerequisites: GCC 14+/Clang 17+ (C++23), CMake 3.22+, Ninja, JDK 17,
Android SDK + NDK r27b for the APK.

```sh
scripts/setup-deps.sh          # fetch vendored deps (googletest, Vulkan headers)
scripts/build_host.sh Release  # native core + tests (ctest)
cd android && ./gradlew assembleDebug
```

Details: docs/development/building.md.

## Testing

```sh
ctest --test-dir build/host --output-on-failure
./gradlew testDebugUnitTest
```

Suite breakdown: docs/development/testing.md.

## Legal requirements

ARMSX5 contains **no games, no firmware, no keys, and no copyrighted system
files**, and implements no access-control circumvention. Users may only
load software they legally own. Development and CI use original homebrew
binaries built in this repository.

## Known limitations

- Integer + SSE subset only: AVX, string ops, privileged ops are reported
  as Unsupported, never mis-executed.
- No guest threading (thr_new → ENOSYS), no interrupt/page-fault delivery
  beyond GuestFault reporting; MXCSR exception flags are not modeled.
- Kernel covers the FreeBSD-style test ABI only; PS5 kernel interfaces are
  unresearched territory.
- GPU: decode/track/probe layers only — no rendering, no shader translation.
- JIT: integer fast paths only; execution requires aarch64 hosts.
- Audio: AAudio backend wired, guest audio engine not integrated yet.
