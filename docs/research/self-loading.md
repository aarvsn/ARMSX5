# SELF Loading — Blocked (Research Notes)

Status: **not implemented, and not implementable within ARMSX5's legal
boundaries today.** This document is referenced by the loader whenever a
SELF container is detected, so users get an honest explanation instead of a
mysterious failure.

## What a SELF is

PS5 executables are distributed as SELF (Sony ELF) containers: an ELF
wrapped with SCE headers, metadata, and encrypted/signed segments. Loading
a retail SELF requires decrypting its segments, which requires key material
derived from console-proprietary secrets.

## Why ARMSX5 does not implement it

1. Legal: decrypting retail SELF content requires key material ARMSX5 must
   not contain or distribute (see docs/research/license-audit.md, red
   lines). Implementing access-control circumvention is out of scope for
   this project.
2. Engineering: even with decrypted segments, PS5 module loading depends on
   kernel services (module registry, code signing policy checks) that do not
   exist yet.

## What ARMSX5 does instead

- `ElfLoader::inspect` detects the SCE magic and returns
  `Status::Unsupported` with a message pointing here — no guessing, no
  partial parsing of encrypted content.
- The loader fully supports plain ELF64 x86-64 executables, which is what
  the project's own homebrew test programs use (tools/test_elfs/*.S), and
  what future legally-obtained homebrew PS5 ELF software would use.

## What reaching PS5 SELF loading would actually require

- A lawful source of decrypted executable segments for a given title
  (the user's own console is the only legitimate origin).
- Kernel emulation of the PS5 module/loader interfaces (sceKernelLoadModule
  family), which depends on Phase-4 kernel work that is still scaffolded.
- Public research references: Kyty's SELF handling (MIT) is the best
  documented open implementation to study; KytyPS5 documents further PS5
  container work (GPL-2.0-only).

## Current loader guarantees

For any input the loader either (a) fully validates and maps a plain ELF64
image, or (b) fails with a precise status and message. It never maps
partially-validated content, and it never executes bytes it could not
validate.
