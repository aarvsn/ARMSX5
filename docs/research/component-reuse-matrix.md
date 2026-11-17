# Component Reuse Matrix

Decisions per ARMSX5 subsystem. "Reference-only" means engineers may read
the upstream implementation for layout/semantics understanding; no code is
copied unless the license column permits it and the integration notes below
are followed.

| Component | Candidate upstream | License | Decision | Integration method | Attribution |
|---|---|---|---|---|---|
| x86-64 decoder/interpreter (integer + SSE) | ARMSX5 original; shadPS4 for reference | n/a / GPL-2.0 | Implement new (done) | — | — |
| ARM64 code emitter | ARMSX5 original; RPCS3 ARM64 backend as prior art | n/a / GPL-2.0 | Implement new (done) | Unit-tested encodings | — |
| ELF64 loader | ARMSX5 original | n/a | Implement new (done) | — | — |
| SELF loading | Kyty (MIT) research | MIT | Blocked (docs/research/self-loading.md) | Future port candidate — MIT is GPL-compatible | Keep Kyty copyright notice |
| PM4 command decode | radeonsi/amdgpu sid.h layout; shadPS4 video core | GPL-2.0 | Implement new, layout documented (done) | Field layout cited, code original | Cite sid.h in pm4.h |
| RDNA2 shader decode | AMD RDNA ISA reference | documentation | Scaffold implemented (format classifier) | Full recompiler is Phase 5/7 work | Cite AMD doc |
| Vulkan backend | Khronos Vulkan-Headers | Apache-2.0 | Vendored include-only (done) | externals/vulkan-headers | Vendored notice retained |
| Kernel/syscalls | FreeBSD syscalls.master (layout facts) | FreeBSD | Reference facts only (done) | Numbers + FreeBSD errno convention cited | Cite FreeBSD source |
| HLE modules | shadPS4 (GPL-2.0) | GPL-2.0 | Framework implemented; module bodies deferred | License-compatible import allowed later | File-level attribution |
| Audio output | AAudio (Android NDK) vs Oboe (Apache-2.0) | Apache-2.0 / platform | Use AAudio directly (done) | Oboe adds a dependency for little gain at our scale; AAudio is what Oboe wraps | — |
| Input mapping | Android GameController docs | documentation | Implement new (done) | Keycode constants cited | — |
| Unit tests | GoogleTest v1.14.0 | BSD-3-Clause | Vendored clone (done) | externals/googletest, add_subdirectory | License file retained |
| Android UI | Jetpack Compose + Material 3 | Apache-2.0 | Use as published (done) | Gradle dependencies | — |
| CI workflows | GitHub documented actions | per-action | actions/checkout@v4, setup-java@v4, gradle/actions/setup-gradle@v3, upload-artifact@v4 | See .github/workflows | — |

## Rules for future imports

1. GPL-2.0-only upstream (shadPS4, KytyPS5, RPCS3) may be imported verbatim;
   imports must carry per-file upstream copyright headers and a NOTICE entry,
   and must record the upstream commit hash in docs/research/upstream-audit.md.
2. MIT/BSD/Apache components may be imported with their license files
   retained; Apache-2.0 additionally requires NOTICE attribution.
3. GPL-3.0-only upstream is NOT acceptable for a GPL-2.0-only project
   without upstream relicensing permission.
4. Never import code sight-unseen from unaudited "PS5 emulator" repositories
   (multiple malware impersonations reported in 2026).
