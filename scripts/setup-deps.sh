#!/usr/bin/env bash
# Fetches vendored dependencies (googletest, Vulkan headers) if absent.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ ! -f externals/googletest/CMakeLists.txt ]; then
    mkdir -p externals
    rm -rf externals/googletest
    git clone -q --depth 1 -b v1.14.0 https://github.com/google/googletest externals/googletest
    echo "vendored googletest v1.14.0"
fi

if [ ! -f externals/vulkan-headers/include/vulkan/vulkan.h ]; then
    mkdir -p externals
    rm -rf externals/vulkan-headers
    git clone -q --depth 1 -b v1.3.290 https://github.com/KhronosGroup/Vulkan-Headers externals/vulkan-headers
    echo "vendored Vulkan-Headers v1.3.290"
fi

echo "dependencies ready"
