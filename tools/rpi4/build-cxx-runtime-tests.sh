#!/usr/bin/env bash
# Build bounded C++ runtime probes for both libstdc++ ABIs.
# Copy the four outputs to a private directory on Haiku and run each
# probe-abiN /absolute/path/plugin-abiN.so. EXPECT_CXX_PROVIDER optionally
# requires a particular loaded library path; LIBRARY_PATH selects a variant.
set -euo pipefail
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$TOOLS/../rock5-itx/env.sh"
ROOT=${1:-/mnt/HaikuWork/rpi4/cxx-runtime/tests}
CROSS=/mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-
CXX_SYSROOT=/mnt/HaikuWork/artifacts/mali-system-opengl-build/20260918T125722Z/sysroot
NATIVE_GCC=/mnt/HaikuWork/rpi4/build/build_packages/gcc_syslibs_devel-13.3.0_2026_03_29_bootstrap-1-arm64/develop/lib/libgcc.a
test "$(sha256sum "$NATIVE_GCC" | cut -d' ' -f1)" = \
    54ec0f159e84a2f8d35a1de4badf7456738d11c70abacc30b239ac1f46f3d94d
mkdir -p "$ROOT"
for abi in 0 1; do
    flags=(--sysroot="$CXX_SYSROOT" -O2 -g -std=c++17 -pthread
        -D_GLIBCXX_USE_CXX11_ABI="$abi" -Wl,--hash-style=both -nodefaultlibs)
    # Use the native shared unwinder. The cross toolchain's default static
    # unwinder cannot safely test exceptions crossing into the native runtime.
    libs=(-lstdc++ -lgcc_s -lroot "$NATIVE_GCC")
    "${CROSS}g++" "${flags[@]}" -shared -fPIC \
        "$TOOLS/cxx_runtime_plugin.cpp" -o "$ROOT/plugin-abi$abi.so" "${libs[@]}"
    "${CROSS}g++" "${flags[@]}" "$TOOLS/cxx_runtime_probe.cpp" \
        -o "$ROOT/probe-abi$abi" "${libs[@]}"
done
sha256sum "$ROOT"/probe-abi? "$ROOT"/plugin-abi?.so
