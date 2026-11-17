# Debugging ARMSX5

## Host CLI

`armsx5-runner` is the primary diagnostic tool:

```sh
./build/host/tests/armsx5-runner <elf> --log trace
```

- `--log trace` prints every executed instruction (RIP, class, length).
- The session report shows exit code, step count, mapped bytes, kernel
  syscall counters, the fault/stop reason, and captured guest stdout.

Exit codes of the runner itself: 3 = load failure, 4 = guest stopped
without exiting; otherwise the guest's exit code (masked to 0-255).

## Log levels

`ARMSX5_LOG_*` macros write to stderr (host) and logcat (`ARMSX5` tag,
Android). Levels: trace, debug, info (default), warn, error. Set via
`armsx5::log::set_level` (native), `nativeSetLogLevel` (JNI), or
`--log` on the runner.

## Common failure patterns

| Symptom | Meaning | Where to look |
|---|---|---|
| `Unsupported` stop | Guest used an unimplemented instruction | trace log names the RIP; decoder reports the opcode |
| `Fault: #DE` | Guest division fault | interpreter div paths |
| `address below any mapping` | Guest fetch/access outside mapped memory | stack/RSP or branch-target bug; trace the last branch |
| `permission denied for access` | Guest W^X violation | loader segment flags vs access |
| `SSE aligned access requires 16-byte alignment` | Aligned SSE move on an unaligned address | matches hardware #GP behavior |
| `SELF container detected` | Encrypted PS5 executable | docs/research/self-loading.md (expected rejection) |
| Vulkan probe error | No loader/ICD on host | expected on headless CI; skip is loud |

## SSE debugging tips

When hand-encoding SSE test bytes, remember:

- RIP-relative displacement counts from the END of the instruction.
- `movd xmm, r32` (66 0F 6E) and `movq r64, xmm` (66 REX.W 0F 7E) have
  opposite operand directions; get the ModRM reg/rm roles right.
- Aligned moves (MOVAPS/MOVAPD/MOVDQA) fault on unaligned addresses by
  design.

## Android

- logcat: `adb logcat -s ARMSX5`.
- The Diagnostics screen surfaces the native device probe JSON and runs the
  built-in homebrew test in-process; failures return a JSON `detail` field.
- Native crashes: the core avoids raw pointers across guest addresses, so
  most guest misbehavior becomes a GuestFault instead of a host crash; if a
  host crash occurs, run the same input through `armsx5-runner` on the host
  to reproduce deterministically.

## JIT

JIT execution requires an aarch64 host; on x86-64 it reports Unsupported by
design. To debug generated code, dump the arena (`Jit` stats expose block
ranges) and disassemble with `llvm-objdump -d --triple=aarch64`. Encoder
unit tests are the first stop for bad encodings.
