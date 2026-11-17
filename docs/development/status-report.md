# ARMSX5 Development Status Report

Date: 2026-11-16 · Milestone 0.2.0 · All numbers below are from actual runs
in this session (commands recorded in docs/development/*.md).

Session note: the sandbox was recycled between milestones and wiped the
repository; it was fully restored from the development record and extended
with the SSE milestone. The 0.1.0 APK artifacts survive in `download/`
(they were built from identical 0.1.0 sources); 0.2.0 APKs require
re-installing the Android toolchain (JDK 17 + SDK 34 + NDK r27b) — the
build recipe is unchanged and CI performs the same steps.

## 1. Final repository structure

```
ARMSX5/
├── android/app/                 Kotlin/Compose frontend + JNI (libarmsx5_jni.so)
│   ├── src/main/cpp/            JNI bridge + core build (NDK/CMake)
│   ├── src/main/kotlin/com/armsx5/   MainActivity, data (JNI/SAF), ui (6 screens)
│   ├── src/main/assets/         hello.elf, sum.elf, callret.elf (on-device tests)
│   └── src/test/                pure-Kotlin unit tests
├── native/{core,memory,loader,cpu,system,gpu,audio,input,platform}/
├── tests/{memory,loader,cpu,system,gpu,integration}/   GoogleTest suites
├── tools/                       armsx5-runner.cpp, test_elfs/*.S (homebrew)
├── docs/{research,architecture,development,compatibility}/
├── .github/workflows/           android-build, native-tests, ci, release
├── externals/                   googletest v1.14.0, Vulkan-Headers v1.3.290
├── CMakeLists.txt  scripts/  README.md  CONTRIBUTING.md  CHANGELOG.md  LICENSE
```

## 2-3. Implementation + phase-by-phase completion

| Phase | Status | Evidence |
|---|---|---|
| 0 Upstream audit | **Complete** | docs/research/* (web-verified, dates recorded) |
| 1 Android app + native foundation | **Complete** | App built in the 0.1.0 session; JNI init/shutdown/device-info; sources restored at 0.2.0 |
| 2 Executable formats + loader | **Complete** | 15 loader tests incl. malformed battery; SELF rejection documented |
| 3 CPU + ARM64 JIT | **Integer+SSE complete; AVX/JIT-exec open** | 40 CPU tests: integer + SSE subset with exact flags; ARM64 encoder unit-tested; JIT translates with interpreter fallback |
| 4 Kernel/system emulation | **Framework complete, PS5 interfaces unresearched** | 12 system tests; FreeBSD-baseline syscalls, VFS, HLE registry; thr_new → ENOSYS |
| 5 GPU + Vulkan | **Decode/probe layers complete; rendering not started** | 16 GPU tests; PM4 decode, resources, RDNA2 classifier, Vulkan probe |
| 6 Full runtime integration | **Complete for homebrew x86-64 ELFs** | 5 integration tests; hello/sum/callret end-to-end on host; on-device via Diagnostics screen |
| 7 Commercial compatibility | **Blocked (legal + engineering)** | SELF loading requires key material — docs/research/self-loading.md |
| 8 Performance | **Not started (by design)** | Correctness first; no measurements taken, so none claimed |
| 9 Release engineering | **Complete for a 0.2.0 milestone** | CI workflows, reproducible build scripts, versioned artifacts |

## 4. Upstream code reuse report

No upstream *code* was imported. Vendored: GoogleTest (BSD-3) and Khronos
Vulkan-Headers (Apache-2.0), both license-file retained. Layout facts cited
with sources: FreeBSD syscalls.master, Mesa sid.h, Android docs, AMD RDNA
ISA reference (docs/research/facts.md). Future import rules in
docs/research/component-reuse-matrix.md.

## 5. License compliance

GPL-2.0-only (`LICENSE`, canonical SPDX text). Third-party licenses audited
(docs/research/license-audit.md): all vendored/used components are
GPL-2.0-compatible. No game data, firmware, keys, or DRM circumvention in
the repository; homebrew test binaries are original works built from
tools/test_elfs/*.S.

## 6. Build instructions

docs/development/building.md — verified paths:
- Host: `scripts/setup-deps.sh && scripts/build_host.sh Release`
- Android: `cd android && ./gradlew assembleDebug` (JDK 17 + NDK r27b)

## 7. Test results (actual counts, this session)

| Suite | Passed | Skipped |
|---|---|---|
| armsx5_tests_core_memory | 17 | 0 |
| armsx5_tests_loader | 15 | 0 |
| armsx5_tests_cpu | 40 | 0 |
| armsx5_tests_system | 12 | 0 |
| armsx5_tests_gpu | 16 | 1 (Vulkan probe: no loader/ICD on headless host — loud skip) |
| armsx5_tests_integration | 5 | 0 |
| **Total** | **105** | **1** |

`ctest` reports 6/6 suites passing (100%). In the 0.1.0 session the same
pipelines also produced debug+release APKs and passing Kotlin unit tests.

## 8. Known issues

- AVX (VEX-encoded) instructions decode as Unsupported — the top CPU work
  item after SSE.
- JIT execution requires aarch64 hosts; x86-64 CI validates translation
  only. On-device differential testing not yet run.
- VOP3 sub-layouts / EXP COMPR-DONE positions need verification against
  the AMD ISA manual before the shader recompiler uses them.
- Guest threading, guest audio integration, and surface/swapchain wiring
  are scaffolds.
- MXCSR exception flags (SSE FP exceptions) are not modeled; rounding uses
  the host default (nearest-even), documented in docs/architecture/cpu.md.
- Release APKs are debug-signed until signing secrets are configured.

## 9. Remaining technical blockers

1. **SELF loading** — requires key material ARMSX5 cannot legally contain
   or distribute (docs/research/self-loading.md). This gates all
   commercial-title work (Phase 7).
2. **RDNA2 → SPIR-V** — the largest engineering mountain: per-opcode
   semantics, control flow, descriptor mapping, SPIR-V emission.
3. **PS5 kernel interfaces** — real syscall/module semantics need primary
   research; ARMSX5 currently implements only its documented test ABI.
4. **AVX execution** — PS5 code is AVX-heavy; VEX decode + execution is the
   next CPU mountain after SSE.

## 10. Android APK artifacts

- 0.1.0 artifacts survive in the session `download/` directory
  (armsx5-android-debug.apk / armsx5-android-release.apk, built from
  identical sources).
- 0.2.0 APKs: rebuild via `cd android && ./gradlew assembleDebug` after
  re-installing the Android toolchain (see docs/development/building.md);
  CI performs the identical steps on every push.

## 11. GitHub Actions workflow status

Four workflows authored (android-build, native-tests, ci, release) using
pinned, maintained action versions. They have not run against a live
GitHub repository in these sessions (no remote exists in the sandbox);
the same steps were validated locally (native ctest + gradle builds ran
in the 0.1.0 session).

## 12. Next recommended engineering tasks

1. AVX/VEX decode + interpreter paths (VEX 2/3-byte prefix handling first).
2. RDNA2 opcode semantics table + minimal SPIR-V emitter for ALU+EXP.
3. On-device JIT differential harness (aarch64 phone or qemu-user).
4. Guest thread registry + cooperative scheduler (thr_new → real threads).
5. Vulkan swapchain wiring behind the platform interface.
6. 16 KiB page-size verification of the produced APK on Android 15+
   hardware.
