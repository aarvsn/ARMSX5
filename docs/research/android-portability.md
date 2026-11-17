# Android Portability Notes

Constraints that shape the ARMSX5 native core for Android (ARM64-v8a).

## Executable memory (W^X)

Android 10 (API 29) removed the ability to create anonymous mappings that
are simultaneously writable and executable (memory objects must be
read-only-executable). The standard workaround — used by V8 and jemalloc —
is dual mapping one `memfd_create` backing object twice:

- one `MAP_SHARED` `PROT_READ|PROT_WRITE` view for the code generator,
- one `MAP_SHARED` `PROT_READ|PROT_EXEC` view for execution.

ARMSX5 implements exactly this in `native/platform/src/exec_arena.cpp` for
`__ANDROID__` builds, with a plain RWX anonymous mapping on desktop
development hosts. The JIT never executes from a writable view on Android.

## Page size

Android 15 devices may use 16 KiB kernel pages; Google requires native apps
to be 16 KiB-aligned for those devices (developer.android.com/guide/
practices/page-sizes). ARMSX5 impact:

- The guest page size is fixed at 4 KiB (x86-64 semantics) and is
  independent of the host page size — guest memory is backed by host
  anonymous mappings, so no guest-visible change is needed.
- Host-side assumptions: `GuestMemory` rounds host allocations by the host
  page size (`sysconf(_SC_PAGESIZE)`), not a hardcoded 4096.
- The Gradle/NDK build must produce 16 KiB-aligned ELF segments when
  targeting Android 15+ devices; NDK r27 + linker defaults handle this, and
  CI must verify with `llvm-elfedit`/`check_elf_alignment` once the release
  pipeline is exercised on such hardware (open item).

## Vulkan

- `libvulkan.so` is part of the Android OS since API 24. ARMSX5 probes it
  with `dlopen` (no link-time dependency) so the core builds and unit-tests
  on hosts without any Vulkan runtime (tests skip with a clear message).
- Device capability reporting (API version, vendor/device IDs, extension
  count) is implemented; actual rendering is not (docs/architecture/gpu.md).
- Adreno and Mali drivers differ in driver-specific behavior; capability-
  based feature selection is mandated (no vendor hardcoding).

## Audio

AAudio (NDK API 26+) is used directly for output. Oboe wraps AAudio and
would add a source dependency; at ARMSX5's current scale the platform API
is sufficient (decision recorded in the reuse matrix).

## Storage

The frontend uses the Storage Access Framework (tree URIs). Guest
executables are imported (copied) into app-private storage before the
native loader touches them — the native core only accepts filesystem paths
and performs full validation before mapping anything.

## Threading

Android enforces no restrictions beyond POSIX on threads/atomics. The
interpreter is single-threaded by design (correctness reference); the JIT
cache uses a thread-local result channel documented in the code.

## Target platform envelope

- minSdk 31 (Android 12+ per project brief), targetSdk 34, ABI arm64-v8a.
- NDK r27b (27.0.12077973), CMake 3.22.1 via sdkmanager, C++23
  (`-std=c++2b`, NDK clang 18).
- No root access and no dangerous permissions are required or requested.
