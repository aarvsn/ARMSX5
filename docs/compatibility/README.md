# Compatibility

**Status: no commercial PS5 title runs on ARMSX5.** This database exists so
that future results can be recorded honestly against reproducible criteria.

## Compatibility categories

| Category | Meaning |
|---|---|
| Not tested | No evidence either way |
| Unsupported | Fails in a way we do not plan to fix soon |
| Loads executable | Loader validated and mapped the binary |
| Reaches initial boot | Guest code executes past the entry point |
| Reaches title screen | UI/framebuffer output exists |
| In-game | Interactive gameplay code runs |
| Playable with issues | Completes gameplay sessions with documented defects |
| Playable | Stable sessions, no known blockers |
| Perfect | Indistinguishable from hardware (highest bar) |

## Tested software

| Name | Type | Result | Build | Notes |
|---|---|---|---|---|
| hello.elf (ARMSX5 homebrew) | x86-64 ELF test program | Playable (deterministic: exits 42, prints greeting) | 0.1.0 / 0.2.0 | Pipeline verified on host CI |
| sum.elf (ARMSX5 homebrew) | x86-64 ELF test program | Playable (exits 5050) | 0.1.0 / 0.2.0 | ALU/flags/branch coverage |
| callret.elf (ARMSX5 homebrew) | x86-64 ELF test program | Playable (exits 12) | 0.1.0 / 0.2.0 | CALL/RET/LEA/stack coverage |
| Commercial PS5 titles | — | Not tested | — | Blocked by SELF loading (docs/research/self-loading.md) and missing kernel/GPU subsystems |

## Rules for entries

- Every entry must state the build version, host/device, and reproduction
  steps (command line or on-device screen).
- Never promote a title's category without reproducible evidence recorded
  in the repository (logs or test output).
- Homebrew entries may be automated in CI; commercial entries additionally
  require a statement of legal provenance for the tested copy.
