#!/usr/bin/env bash
# Build standalone pixel checks against the same pinned EGL/GLES sysroot.
set -euo pipefail
BASE=${RPI4_MESA_BASE:-/mnt/HaikuWork/artifacts/mali-system-opengl-build/20260918T125722Z}
ROOT=${RPI4_MESA_ROOT:-/mnt/HaikuWork/rpi4/mesa}
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CXX=${RPI4_CXX:-/mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-g++}
mkdir -p "$ROOT/probes"
for probe in gl texture; do
    "$CXX" --sysroot="$BASE/sysroot" -std=c++17 -O2 \
        -I"$BASE/glvnd-install/boot/system/develop/headers/os/opengl" \
        -I"$BASE/sysroot/boot/system/develop/headers/os/opengl" \
        "$TOOLS/${probe}_probe.cpp" -L"$BASE/glvnd-install/boot/system/lib" \
        -lEGL -lGLESv2 -o "$ROOT/probes/rpi4_${probe}_probe"
done
