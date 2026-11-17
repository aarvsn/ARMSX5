# Verified Facts

Every claim below was verified via web search on 2026-11-16 unless marked
UNVERIFIED. Citations are given as the source actually consulted.

## FreeBSD/amd64 syscall numbers

Source: freebsd-src `sys/kern/syscalls.master`
(https://github.com/freebsd/freebsd-src/blob/main/sys/kern/syscalls.master),
cross-checked with the Go runtime's `sys_freebsd_amd64.s`
(https://go.dev/src/runtime/sys_freebsd_amd64.s):

- exit=1, read=3, write=4, open=5, close=6, munmap=73, mprotect=74,
  sched_yield=331, thr_new=455, mmap=477.

FreeBSD amd64 error convention: on failure the carry flag is set and the
errno value is returned in RAX. ARMSX5's kernel implements this convention
(native/system/kernel.cpp). PS5 kernel numbering diverges from stock
FreeBSD for some calls; ARMSX5 uses the stock FreeBSD baseline for its own
test ABI and documents the divergence as a research item — this is
explicitly NOT a claim about PS5 kernel interfaces.

## PS5 OS is FreeBSD-based

Reported across public PS5 emulation research (Kyty documentation,
community write-ups). Exact kernel version and divergences: UNVERIFIED;
requires primary research before any kernel-emulation work beyond ARMSX5's
test ABI.

## AMD PM4 Type-3 packet header

Source: Linux amdgpu / Mesa radeonsi `sid.h` PACKET3 macro
(https://github.com/mesa3d/mesa/blob/main/src/amd/common/sid.h):

- bits [31:30] = packet type (3),
- bits [29:16] = body dword count (14 bits),
- bits [15:8]  = opcode.

`PACKET3(op, n)` encodes `n` body dwords after the header. shadPS4's video
core uses this packet family for PS4. PS5 firmware opcodes are a superset;
unknown opcodes are recorded, never interpreted (native/gpu/pm4).

## AMD RDNA2 ISA reference

AMD RDNA ISA reference document is published at developer.amd.com
(https://developer.amd.com/resources/developer-guides-manuals/). The
format bit-layouts implemented in native/gpu/shader/rdna2.cpp (SOP1/SOP2/
SOPK/VOP1/VOP2/EXP prefixes and field positions) follow that reference;
VOP3 sub-layouts and EXP COMPR/DONE positions still require verification
against the manual before the shader recompiler may rely on them (noted in
code and tests).

## Android W^X restriction

Android 10+ prevents apps from mapping writable+executable anonymous
memory; the memfd_create dual-mapping technique is the accepted workaround
(Android NDK documentation and AOSP behavior; used by V8 and jemalloc).
ARMSX5 implements this in native/platform/exec_arena.cpp.

## Android 16 KiB page size

Google documents 16 KiB page-size devices from Android 15 and the
requirement that native libraries support them
(https://developer.android.com/guide/practices/page-sizes). NDK r27+
toolchains produce compatible output; CI verification on real 16 KiB
hardware is an open item.

## Android gamepad event model

Source: developer.android.com (Game controllers): button keycodes
(KEYCODE_BUTTON_A=96, BUTTON_B=97, BUTTON_X=99, BUTTON_Y=100,
BUTTON_L1=102, BUTTON_R1=103, BUTTON_THUMBL=110, BUTTON_THUMBR=111,
BUTTON_START=108, BUTTON_SELECT=109, DPAD 19-22); analog axes come through
MotionEvent (AXIS_X=0, AXIS_Y=1, AXIS_Z=11, AXIS_RX=14, AXIS_BRAKE=23,
AXIS_GAS=22). Implemented in native/input/controller.cpp.

## Toolchain versions (pinned for ARMSX5)

Verified against official download endpoints on 2026-11-16:

- Android Gradle Plugin 8.5.2 (Google Maven), Gradle 8.9
  (services.gradle.org), Kotlin 2.0.20 (Maven Central),
- Android SDK: platform 34, build-tools 34.0.0, cmake 3.22.1, NDK r27b
  (27.0.12077973) via dl.google.com sdkmanager,
- GoogleTest v1.14.0, Khronos Vulkan-Headers v1.3.290 (GitHub releases).

## Upstream projects

See upstream-audit.md for the full per-project audit with observation dates
and license verification.
