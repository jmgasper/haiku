#!/usr/bin/env bash
# Build the pinned Rockchip MPP core library for Haiku arm64.
set -euo pipefail
source "$(dirname "$0")/env.sh"

revision=14729dd578e570e5f00fd1dd2113f5429012d64b
cache="$HAIKU_WORK/src/rockchip-mpp"
root="$HAIKU_WORK/build/rock5-mpp-arm64/repro"
source_dir="$root/source"
build_dir="$root/build"
stage="$HAIKU_WORK/artifacts/ffmpeg-arm64/mpp-stage"
patch="$HAIKU_SOURCE/tools/rock5-itx/mpp/mpp-14729dd-haiku.patchset"
compat="$HAIKU_SOURCE/tools/rock5-itx/mpp/compat"
sysroot="$HAIKU_WORK/artifacts/mesa-reconstruction/20260915T131119Z-96b4bc/sysroot"
cross="$HAIKU_WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-"

if [[ ! -d "$cache/.git" ]]; then
    git clone --filter=blob:none --no-checkout https://github.com/rockchip-linux/mpp.git "$cache"
    git -C "$cache" fetch --depth 1 origin "$revision"
    git -C "$cache" checkout --detach "$revision"
fi
[[ "$(git -C "$cache" rev-parse HEAD)" = "$revision" ]] || {
    echo "MPP source cache has a different revision" >&2
    exit 1
}

mkdir -p "$root" "$stage"
patch_hash=$(sha256sum "$patch")
patch_hash=${patch_hash%% *}
if [[ ! -f "$source_dir/.rock5-patch-sha256" ]] \
    || [[ "$(cat "$source_dir/.rock5-patch-sha256")" != "$patch_hash" ]]; then
    rm -rf "$source_dir" "$build_dir"
    mkdir -p "$source_dir"
    git -C "$cache" archive "$revision" | tar -x -C "$source_dir"
    git -C "$source_dir" apply --check "$patch"
    git -C "$source_dir" apply "$patch"
    printf '%s\n' "$patch_hash" > "$source_dir/.rock5-patch-sha256"
fi

toolchain="$root/haiku-toolchain.cmake"
cat > "$toolchain" <<EOF
set(CMAKE_SYSTEM_NAME Haiku)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT "$sysroot")
set(CMAKE_C_COMPILER "${cross}gcc")
set(CMAKE_CXX_COMPILER "${cross}g++")
set(CMAKE_FIND_ROOT_PATH "\${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

cmake -S "$source_dir" -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TEST=OFF -DBUILD_SHARED_LIBS=ON \
    "-DCMAKE_C_FLAGS=-I$compat -include $compat/asm/ioctl.h" \
    "-DCMAKE_CXX_FLAGS=-I$compat -include $compat/asm/ioctl.h" \
    '-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--no-undefined'
cmake --build "$build_dir" --target rockchip_mpp -j"$HAIKU_JOBS"
cp "$build_dir/mpp/librockchip_mpp.so.0" "$stage/librockchip_mpp.so.1"
for probe in mpp_soc_probe mpp_buffer_probe mpp_group_probe mpp_device_probe \
        mpp_decode_probe; do
    "${cross}gcc" --sysroot="$sysroot" -std=gnu11 -O2 \
        -I"$source_dir/osal/inc" -I"$source_dir/osal" \
        -I"$source_dir/inc" -I"$source_dir/mpp/inc" \
        -I"$source_dir/mpp/base/inc" \
        -o "$stage/$probe" "$HAIKU_SOURCE/tools/rock5-itx/mpp/$probe.c" \
        -L"$build_dir/mpp" -Wl,-rpath,/boot/home/rock5-lab \
        -lrockchip_mpp
done
sha256sum "$stage/librockchip_mpp.so.1"
