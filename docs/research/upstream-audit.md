# Upstream Audit

Audited: 2026-11-16, via live web searches of GitHub and project sites.
Every item reflects what was actually observed on that date; anything that
could not be verified is marked UNVERIFIED. ARMSX5 does not import upstream
code at this stage; this audit drives future reuse decisions.

## Kyty (InoriRus/Kyty)

- Repository: https://github.com/InoriRus/Kyty
- License: MIT (verified from repository README/license badge).
- Status: early stage; "possible to run some simple games for PS4 and
  homebrews for PS5". Windows-only build (VS/clang-cl or Eclipse CDT +
  MinGW), Qt 5.15, Vulkan SDK dependencies. Last release v0.2.0 (Aug 2022);
  low ongoing activity (observed 2026-11-16).
- Implementation state (as documented by the project): SELF handling present
  (PS4/PS5 homebrews boot); kernel/HLE partial and Windows-specific; GPU/PM4
  partial; x86-64 recompilation/interpretation on x86-64 hosts; no Android;
  audio not implemented (self-declared).
- Relevance: the most direct public PS5-homebrew-loading research;
  architectural reference only (MIT, Windows-bound, inactive).

## KytyPS5 (KytyPS5/KytyPS5)

- Repository: https://github.com/KytyPS5/KytyPS5
- License: GPL-2.0-only (self-declared; original Kyty MIT notice preserved
  in LICENSES/Kyty-MIT.txt — verified from repository README).
- Status: very active ("PlayStation 5 translation layer", 11.2k stars,
  147 contributors, 582 releases, latest Nov 12 2026 — observed
  2026-11-16). Targets Windows/Linux/macOS; no Android.
- Relevance: license-compatible candidate for future PM4/HLE reference
  reading; heavy desktop dependencies limit direct reuse.

## shadPS4 (shadps4-emu/shadPS4)

- Repository: https://github.com/shadps4-emu/shadPS4
- License: GPL-2.0 (verified from repository/wikipedia).
- Status: the most mature open PS4-family emulator: runs a subset of
  commercial PS4 titles (e.g. Bloodborne, Dark Souls Remastered) to varying
  degrees; active releases through 2026 (v0.16.0+); Windows/Linux/macOS
  (Apple Silicon), x64 FreeBSD added in 2026. No Android platform.
- Implementation state: x86-64 guest on x86-64 hosts; GPU: PM4/GNM command
  processing + RDNA-family shader recompiler to SPIR-V/Vulkan; HLE modules
  for PS4 system libraries; GoogleTest-based test suite added in 2026.
- Relevance: GPL-2.0-compatible reference for PM4 decoding, GNM HLE
  structure, and shader-recompiler architecture. Not directly portable to
  ARM64/Android.

## LSX4 (lpfoj/lsx4)

- Repository: https://github.com/lpfoj/lsx4
- License: GPL-2.0 (verified from repository).
- Status: small project (54 stars, 83 commits observed 2026-11-16); public
  site lsx4.app describes PS4+PS5 emulation on Android and iOS (ARM64,
  Vulkan). Architecture document describes an in-process x86-64-to-AArch64
  JIT ("Funnel ARM" for GPU/shader translation). Claims of playable PS5
  titles UNVERIFIED by ARMSX5.
- Relevance: the closest public prior art to ARMSX5's exact goal.

## Lumen Emu (gs-soft-it/lumen-emu)

- Repository: https://github.com/gs-soft-it/lumen-emu (per the project's own
  legal/about pages, lumen-emu.com).
- License: UNVERIFIED at audit time.
- Status: experimental PS4/PS5 emulator for Android/iOS, C++23, built on
  cores derived from shadPS4, Kyty and KytyPS5 (per project about page).
  Implementation depth UNVERIFIED.

## Other items checked

- "AnyPS5", "ZeraPS5" and various "PS5 emulator" repositories:
  low-credibility or AI-generated projects; treated as noise. Security
  researchers reported malware impersonating PS5 emulators (PCMag,
  2026-10-29) — only trust the repos audited here.
- RPCS3 (rpcs3/rpcs3): GPL-2.0; its ARM64 recompiler backends are the
  strongest public prior art for AArch64 JIT emulation on Android-class
  hardware.

## Verdict for ARMSX5

No public PS5 emulator currently ships an Android/ARM64 target, which
confirms ARMSX5's niche. Short-term reuse policy: read-only reference use of
shadPS4 (PM4/GNM layout, recompiler structure) and RPCS3 (AArch64 codegen
prior art); same-license (GPL-2.0-only) code import is license-safe but is
deferred until the interfaces it would plug into exist. Kyty remains the
reference for SELF-container research.
