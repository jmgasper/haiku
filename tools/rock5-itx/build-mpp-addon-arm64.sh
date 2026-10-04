#!/usr/bin/env bash
# Builds only the RK3588 Media Kit decoder add-on (00_rockchip_mpp) for arm64,
# with the same flags as build-ffmpeg-arm64.sh, for trying a change on the
# board without rebuilding FFmpeg and MPP:
#   tools/rock5-itx/build-mpp-addon-arm64.sh [output directory]
set -euo pipefail
WORK=/mnt/HaikuWork
SOURCE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${1:-$WORK/tmp/rock5-mpp-addon}
sysroot=$WORK/artifacts/mesa-reconstruction/20260915T131119Z-96b4bc/sysroot
cross=$WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-
ffmpeg=$WORK/artifacts/ffmpeg-arm64/stage/boot/system/non-packaged
mpp_source=$WORK/build/rock5-mpp-arm64/repro/source
mpp_build=$WORK/build/rock5-mpp-arm64/repro/build
media=$WORK/build/arm64/objects/haiku/arm64/release/kits/media
mkdir -p "$OUT"
"${cross}g++" --sysroot="$sysroot" -std=gnu++17 -O2 -fPIC \
	-Wall -Wextra -Werror -D__STDC_CONSTANT_MACROS \
	-I"$ffmpeg/include" \
	-iquote "$ffmpeg/include/libavcodec" \
	-iquote "$ffmpeg/include/libavutil" \
	-iquote "$ffmpeg/include/libswscale" \
	-I"$SOURCE/headers/private/media" \
	-I"$SOURCE/headers/private/shared" \
	-I"$mpp_source/inc" -I"$mpp_source/mpp/inc" \
	-c "$SOURCE/tools/rock5-itx/RockchipMppDecoder.cpp" \
	-o "$OUT/RockchipMppDecoder.o"
"${cross}g++" --sysroot="$sysroot" -shared -o "$OUT/00_rockchip_mpp" \
	"$OUT/RockchipMppDecoder.o" -L"$mpp_build/mpp" -L"$ffmpeg/lib" -L"$media" \
	-Wl,-rpath-link,"$sysroot/boot/system/lib" \
	-Wl,-rpath-link,"$ffmpeg/lib" -Wl,--no-undefined \
	-lrockchip_mpp -lavcodec -lswscale -lavutil -lbe -lmedia -lsupc++
echo "$OUT/00_rockchip_mpp"
