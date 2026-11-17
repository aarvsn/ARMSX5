# Contributing to ARMSX5

## Ground rules

1. **Correctness before performance.** The interpreter is the reference;
   the JIT must match it exactly.
2. **No fake implementations.** Unsupported features must fail loudly with
   documented status codes. Never fabricate success, results, or
   compatibility claims.
3. **Tests travel with code.** New subsystems land with GoogleTest
   coverage; bug fixes add regression tests.
4. **Honest documentation.** Docs must distinguish implemented/scaffolded/
   blocked. The compatibility database accepts reproducible evidence only.
5. **Licensing.** GPL-2.0-only. See docs/research/license-audit.md before
   importing any third-party code; never commit game data, firmware, keys,
   or DRM circumvention.
6. **Portable core stays portable.** Android specifics belong behind
   interfaces (platform/, JNI); the core builds and tests on desktop Linux.

## Workflow

1. Fork + branch (`feature/<topic>`).
2. `scripts/build_host.sh Release` must pass (105 tests at time of writing).
3. `cd android && ./gradlew assembleDebug testDebugUnitTest` if the frontend
   changed.
4. Update docs for any behavior change; add a CHANGELOG entry.
5. PRs: describe the change, the tests added, and any new limitations.
   CI (GitHub Actions) builds the APK and runs native tests on every PR.

## Code style

- C++23, no external framework deps in the core; `std::expected` for
  recoverable errors, `Status`/`Error` for cross-boundary reporting.
- Kotlin: official style, Compose + Material 3, no deprecated APIs.
- Keep functions small enough to test in isolation; name things after what
  they do, not how.

## Where to work first (up to date with the status report)

- AVX/VEX decode + interpreter paths (the top CPU item now that SSE landed).
- RDNA2 opcode semantics + SPIR-V emission prototype.
- Guest threading design (thr_new/scheduling).
- On-device JIT differential testing harness.
