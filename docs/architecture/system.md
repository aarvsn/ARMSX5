# System Architecture

## Kernel (`native/system/kernel.cpp`)

Syscall dispatch over the documented FreeBSD/amd64 baseline (numbers cited
in docs/research/facts.md): exit, write, read (stub → EBADF), mmap, munmap,
mprotect, sched_yield. Error convention: CF=1 + errno in RAX (FreeBSD
amd64), with FreeBSD errno values. Every unsupported syscall returns
ENOSYS and is counted in `KernelStats` — no fabricated success paths.

`exit` places the exit code in RAX (the run loop reports it), records it in
the kernel, and stops execution.

## Memory management integration

mmap/munmap/mprotect map onto `GuestMemory::map/unmap/protect`. The kernel
therefore inherits bounds and overlap enforcement (unaligned hints fall
back to the bump allocator; zero-length or oversized requests fail with
EINVAL/ENOMEM).

## VFS (`native/system/vfs.{h,cpp}`)

- `MemFS` — in-memory backend for tests/CI.
- `HostDirectoryFS` — rooted host-directory backend with strict path
  traversal protection (rejects absolute paths, `..`, NUL bytes; resolves
  only regular files under the root). Android SAF integration plugs into
  the same interface through the frontend's import step.

## HLE registry (`native/system/module_registry.{h,cpp}`)

Modules are registered by name with export lists and an implemented-count.
Calls to unimplemented exports are recorded as "unimplemented traps" and
surfaced as errors — the registry never invents return values. No PS5
libSce* module is registered today; the framework and its tests are the
Phase-4 deliverable, with module bodies dependent on real interface
research (docs/research/facts.md notes what is verified).

## Process runtime (`native/system/process.{h,cpp}`)

The end-to-end pipeline: file → `ElfLoader::inspect/load` → guest stack
allocation → interpreter (optional JIT on aarch64) → kernel. Produces a
`RunResult` with exit code, step count, captured stdout, kernel stats, and
fault diagnostics; consumed by both `armsx5-runner` (host CLI) and the
Android frontend (JNI).

## Guest threading

Not implemented. `thr_new` (and any other thread-creating syscall) returns
ENOSYS. The design keeps the interpreter single-threaded as the correctness
reference; cooperative scheduling is the planned approach once guest
thread state is defined.
