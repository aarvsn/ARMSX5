#!/usr/bin/env bash
# Syncs the built homebrew test ELFs into the Android app assets.
# Usage: scripts/sync_assets.sh [build-dir]
set -euo pipefail
BUILD=${1:-build/host}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
for f in hello.elf sum.elf callret.elf simdavx.elf; do
    if [ ! -f "$BUILD/test_elfs/$f" ]; then
        echo "missing $BUILD/test_elfs/$f — build the armsx5_test_elfs target first" >&2
        exit 1
    fi
    cp "$BUILD/test_elfs/$f" "$ROOT/android/app/src/main/assets/$f"
done
ls -la "$ROOT/android/app/src/main/assets/"
echo "assets synced"
