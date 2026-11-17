# License Audit

Project license: **GPL-2.0-only** (canonical text vendored at `LICENSE`,
SPDX `GPL-2.0-only`). The `LICENSE` file contains the verbatim GNU GPLv2
text (obtained from the SPDX license list, which distributes license texts
under CC0).

## Compatibility matrix (as of 2026-11-16)

| Component | License | GPL-2.0-only compatible? | Action |
|---|---|---|---|
| GoogleTest v1.14.0 (vendored) | BSD-3-Clause | Yes | Keep license file in clone; mention in NOTICE |
| Khronos Vulkan-Headers v1.3.290 (vendored) | Apache-2.0 | Yes, with attribution | Keep LICENSES/ from the clone; NOTICE entry |
| AAudio (Android platform) | Android/AOSP (Apache-2.0 family) | Yes (system library) | No distribution obligation beyond AOSP notice |
| Jetpack Compose / AndroidX | Apache-2.0 | Yes | Standard NOTICE |
| shadPS4 (if code is imported later) | GPL-2.0-only | Yes (same license) | Per-file upstream headers + commit record |
| Kyty (if SELF research is ported) | MIT | Yes | Retain Kyty copyright/license notice |
| KytyPS5, LSX4 | GPL-2.0-only | Yes (same license) | Per-file attribution if imported |
| RPCS3 | GPL-2.0-only | Yes (same license) | Per-file attribution if imported |
| Any GPL-3.0-only component | GPL-3.0-only | **No** | Forbidden without relicensing permission |
| Any proprietary firmware/keys/game data | — | **No** | Forbidden outright |

## Required repository notices

- `LICENSE` — full GPL-2.0 text (present).
- Third-party vendored trees keep their own license files
  (`externals/googletest/LICENSE`, `externals/vulkan-headers/LICENSE.md`).
- `docs/research/upstream-audit.md` records every upstream project consulted
  and the observation date.

## Legal red lines (enforced by project policy)

ARMSX5 is an emulator research project, not a content platform:

1. No commercial game files, PS5 firmware, decrypted modules, or proprietary
   system libraries may be committed, bundled, or downloaded by the software.
2. No private keys, decryption tooling, or access-control circumvention.
   Loading encrypted PS5 SELF executables requires key material ARMSX5
   does not and will not distribute; this is a documented blocker
   (docs/research/self-loading.md), not a planned work item.
3. Development and CI use only original homebrew test binaries built from
   `tools/test_elfs/*.S` in this repository.
4. The compatibility database records honest results only; ARMSX5 must never
   advertise a title as supported without reproducible evidence.
