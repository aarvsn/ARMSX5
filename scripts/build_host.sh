#!/usr/bin/env bash
# Builds the ARMSX5 native core and runs the host test suites.
# Usage: scripts/build_host.sh [Debug|Release]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_TYPE=${1:-Release}
BUILD=$ROOT/build/host

command -v ninja >/dev/null 2>&1 || export PATH="$HOME/.venv/bin:$PATH"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DARMSX5_BUILD_TESTS=ON
ninja -C "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure
echo "host build + tests OK"
