#!/usr/bin/env bash
# Build FFmpeg and Haiku's Media Kit add-on for the arm64 full image.
set -euo pipefail
source "$(dirname "$0")/env.sh"

version=6.1.6
source_hash=d4fcb164028dd3beee5d92c0ac72e46aac6973c75ea12dc14de07bf8f407370a
archive="$HAIKU_WORK/artifacts/ffmpeg-arm64/ffmpeg-$version.tar.xz"
source_dir="$HAIKU_WORK/build/rock5-ffmpeg-arm64/ffmpeg-$version"
stage="$HAIKU_WORK/artifacts/ffmpeg-arm64/stage"
plugin_build="$HAIKU_WORK/build/rock5-ffmpeg-plugin"
sysroot="$HAIKU_WORK/artifacts/mesa-reconstruction/20260915T131119Z-96b4bc/sysroot"
cross="$HAIKU_WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-"
mpp_source="$HAIKU_WORK/build/rock5-mpp-arm64/repro/source"
mpp_build="$HAIKU_WORK/build/rock5-mpp-arm64/repro/build"

"$HAIKU_SOURCE/tools/rock5-itx/build-mpp-arm64.sh"

mkdir -p "$(dirname "$archive")" "$(dirname "$source_dir")" "$plugin_build"
if [[ ! -s "$archive" ]]; then
    curl -fL --retry 3 "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz" -o "$archive"
fi
printf '%s  %s\n' "$source_hash" "$archive" | sha256sum --check --status
if [[ ! -f "$source_dir/configure" ]]; then
    tar -C "$(dirname "$source_dir")" -xf "$archive"
    (cd "$source_dir" && git apply "$HAIKU_SOURCE/tools/rock5-itx/ffmpeg-$version.patchset")
fi

cd "$source_dir"
if [[ ! -f config.mak ]]; then
    ./configure --prefix=/boot/system/non-packaged --target-os=haiku \
        --arch=aarch64 --enable-cross-compile --cross-prefix="$cross" \
        --sysroot="$sysroot" --disable-programs --disable-doc \
        --disable-static --enable-shared --enable-pic --disable-avdevice \
        --disable-network --disable-autodetect --enable-pthreads --enable-small
fi
make -j"$HAIKU_JOBS"
make install DESTDIR="$stage"

ffmpeg="$stage/boot/system/non-packaged"
cd "$plugin_build"
for file in AVCodecDecoder AVCodecEncoder AVFormatReader AVFormatWriter \
        CodecTable DemuxerTable EncoderTable FFmpegPlugin MuxerTable \
        CpuCapabilities gfx_conv_c gfx_conv_c_lookup gfx_util; do
    "${cross}g++" --sysroot="$sysroot" -std=gnu++17 -O2 -fPIC \
        -D__STDC_CONSTANT_MACROS -Wdeprecated -I"$ffmpeg/include" \
        -iquote "$ffmpeg/include/libavcodec" \
        -iquote "$ffmpeg/include/libavformat" \
        -iquote "$ffmpeg/include/libavfilter" \
        -iquote "$ffmpeg/include/libavutil" \
        -iquote "$ffmpeg/include/libswscale" \
        -iquote "$ffmpeg/include/libswresample" \
        -I"$HAIKU_SOURCE/headers/private/media" \
        -I"$HAIKU_SOURCE/headers/private/media/experimental" \
        -I"$HAIKU_SOURCE/headers/private/shared" \
        -c "$HAIKU_SOURCE/src/add-ons/media/plugins/ffmpeg/$file.cpp" -o "$file.o"
done
rm -f RockchipMppDecoder.o
"${cross}g++" --sysroot="$sysroot" -shared -o ffmpeg ./*.o \
    -L"$ffmpeg/lib" \
    -L"$HAIKU_WORK/build/arm64/objects/haiku/arm64/release/kits/media" \
    -Wl,-rpath-link,"$sysroot/boot/system/lib" \
    -Wl,-rpath-link,"$ffmpeg/lib" \
    -lavformat -lavcodec -lavfilter -lswscale -lswresample -lavutil \
    -lbe -lmedia -lsupc++

"${cross}g++" --sysroot="$sysroot" -std=gnu++17 -O2 -fPIC \
    -Wall -Wextra -Werror -D__STDC_CONSTANT_MACROS \
    -I"$ffmpeg/include" \
    -iquote "$ffmpeg/include/libavcodec" \
    -iquote "$ffmpeg/include/libavutil" \
    -iquote "$ffmpeg/include/libswscale" \
    -I"$HAIKU_SOURCE/headers/private/media" \
    -I"$HAIKU_SOURCE/headers/private/shared" \
    -I"$mpp_source/inc" -I"$mpp_source/mpp/inc" \
    -c "$HAIKU_SOURCE/tools/rock5-itx/RockchipMppDecoder.cpp" \
    -o RockchipMppDecoder.o
"${cross}g++" --sysroot="$sysroot" -shared -o 00_rockchip_mpp \
    RockchipMppDecoder.o -L"$mpp_build/mpp" -L"$ffmpeg/lib" \
    -L"$HAIKU_WORK/build/arm64/objects/haiku/arm64/release/kits/media" \
    -Wl,-rpath-link,"$sysroot/boot/system/lib" \
    -Wl,-rpath-link,"$ffmpeg/lib" -Wl,--no-undefined \
    -lrockchip_mpp -lavcodec -lswscale -lavutil -lbe -lmedia -lsupc++

"${cross}g++" --sysroot="$sysroot" -std=gnu++17 -O2 -pthread \
    -o Rock5MediaPlayer "$HAIKU_SOURCE/tools/rock5-itx/Rock5MediaPlayer.cpp" \
    -L"$HAIKU_WORK/build/arm64/objects/haiku/arm64/release/kits/media" \
    -Wl,-rpath-link,"$sysroot/boot/system/lib" \
    -lbe -lmedia -lsupc++
"${cross}g++" --sysroot="$sysroot" -std=gnu++17 -O2 \
    -o media-track-probe "$HAIKU_SOURCE/tools/rock5-itx/media-track-probe.cpp" \
    -L"$HAIKU_WORK/build/arm64/objects/haiku/arm64/release/kits/media" \
    -Wl,-rpath-link,"$sysroot/boot/system/lib" \
    -lbe -lmedia -lsupc++
"${cross}g++" --sysroot="$sysroot" -std=gnu++17 -O2 \
    -I"$HAIKU_SOURCE/src/add-ons/kernel/drivers/graphics/rk3588_display" \
    -o vpu-domain-probe "$HAIKU_SOURCE/tools/rock5-itx/vpu_domain_probe.cpp" \
    -lbe -lsupc++
"${cross}g++" --sysroot="$sysroot" -std=gnu++17 -O2 \
    -o vpu-resource-probe "$HAIKU_SOURCE/tools/rock5-itx/vpu_resource_probe.cpp" \
    -lbe -lsupc++

"$HAIKU_SOURCE/tools/rock5-itx/package-ffmpeg-arm64.sh"
