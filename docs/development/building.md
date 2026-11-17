# Building ARMSX5

## Toolchain (pinned, verified 2026-11-16)

| Component | Version |
|---|---|
| Host compiler (Linux) | GCC 14 (C++23) or Clang 17+ |
| Android NDK | r27b (27.0.12077973) |
| Android Gradle Plugin | 8.5.2 |
| Gradle | 8.9 |
| Kotlin | 2.0.20 (Compose compiler plugin 2.0.20) |
| Android SDK | platform 34, build-tools 34.0.0, CMake 3.22.1 |
| JDK | 17 (Temurin recommended) |
| Unit tests | GoogleTest v1.14.0 (vendored) |

Vendored dependencies live in `externals/` (gitignored clones):
`scripts/setup-deps.sh` fetches them (googletest v1.14.0,
KhronosGroup/Vulkan-Headers v1.3.290) if absent.

## Native core (host, Linux)

```sh
scripts/setup-deps.sh            # one-time: clone vendored deps
scripts/build_host.sh Release    # configure + build + ctest
```

Manual equivalent:

```sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DARMSX5_BUILD_TESTS=ON
ninja -C build/host
ctest --test-dir build/host --output-on-failure
```

The homebrew test ELFs (tools/test_elfs/*.S) are built automatically with
the host compiler and consumed by the integration tests.

## armsx5-runner (host CLI)

```sh
./build/host/tests/armsx5-runner build/host/test_elfs/hello.elf --log debug
```

Loads a plain ELF64, runs it through the full pipeline, prints a session
report (exit code, steps, kernel stats, captured stdout).

## Android APK

```sh
export JAVA_HOME=/path/to/jdk17
export ANDROID_HOME=/path/to/android-sdk
cd android
./gradlew assembleDebug        # or assembleRelease
```

Outputs land in `app/build/outputs/apk/<variant>/`. The native core is
compiled by AGP via CMake (NDK r27b, arm64-v8a only) into
`libarmsx5_jni.so`; the homebrew test ELFs are packaged as assets by
`scripts/sync_assets.sh` (run after building the native tests once).

Release APKs are debug-signed until signing keys are configured via CI
secrets (`ARMSX5_SIGNING_KEYSTORE` etc.); this difference is documented and
intentional.

## Verified build matrix

- Debian Linux x86_64, GCC 14.2, CMake 3.31: native core + all tests
  (verified 2026-11-16 in both milestone sessions).
- Same machine with NDK r27b: `assembleDebug` and `assembleRelease` APKs
  (libarmsx5_jni.so for arm64-v8a + test-ELF assets) — verified in the
  0.1.0 session.
- CI replicates both paths (see .github/workflows).
