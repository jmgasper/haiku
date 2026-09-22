#!/usr/bin/env bash
# Supply the regular arm64 image's MIDI dependency until HaikuPorts has one.
set -euo pipefail
source "$(dirname "$0")/env.sh"

revision=e64b4b3161cab212fffe7d1d3fb1c05750c363cc
archive="$HAIKU_WORK/cache/fluidlite-$revision.tar.gz"
expected=b6c4bff689bfd2b8e57b771640da019ee6cf3580a96ddced3954f9f84b66410f
root="$HAIKU_WORK/build/fluidlite-arm64"
sdk="$HAIKU_WORK/artifacts/mesa-reconstruction/20260915T131119Z-96b4bc/sysroot"
output="$HAIKU_WORK/rock5-image-extras/develop/fluidlite"

mkdir -p "$(dirname "$archive")" "$root"
if [[ ! -f "$archive" ]]; then
    curl -L --fail --silent --show-error \
        "https://github.com/divideconcept/FluidLite/archive/$revision.tar.gz" \
        -o "$archive"
fi
printf '%s  %s\n' "$expected" "$archive" | sha256sum -c -
if [[ ! -d "$root/FluidLite-$revision" ]]; then
    tar -xzf "$archive" -C "$root"
fi
cat > "$root/haiku-arm64.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Haiku)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER $HAIKU_WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-gcc)
set(CMAKE_CXX_COMPILER $HAIKU_WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-g++)
set(CMAKE_SYSROOT $sdk)
set(CMAKE_FIND_ROOT_PATH $sdk)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
cmake -S "$root/FluidLite-$revision" -B "$root/build" \
    -DCMAKE_TOOLCHAIN_FILE="$root/haiku-arm64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DFLUIDLITE_BUILD_STATIC=ON -DFLUIDLITE_BUILD_SHARED=OFF \
    -DENABLE_SF3=ON -DSTB_VORBIS=ON \
    -DCMAKE_INSTALL_PREFIX="$root/install"
cmake --build "$root/build" -j"$HAIKU_JOBS"
cmake --install "$root/build"
mkdir -p "$output"
cp -a "$root/install/include" "$output/"
cp -a "$root/install/lib" "$output/"
sha256sum "$output/lib/libfluidlite.a"
