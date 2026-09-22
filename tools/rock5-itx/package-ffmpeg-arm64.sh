#!/usr/bin/env bash
# Package the locally cross-built FFmpeg libraries and Haiku Media Kit add-on.
set -euo pipefail
source "$(dirname "$0")/env.sh"

cross="$HAIKU_WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-"
ffmpeg="$HAIKU_WORK/artifacts/ffmpeg-arm64/stage/boot/system/non-packaged"
plugin="$HAIKU_WORK/build/rock5-ffmpeg-plugin/ffmpeg"
mpp_plugin="$HAIKU_WORK/build/rock5-ffmpeg-plugin/00_rockchip_mpp"
mpp_library="$HAIKU_WORK/artifacts/ffmpeg-arm64/mpp-stage/librockchip_mpp.so.1"
player="$HAIKU_WORK/build/rock5-ffmpeg-plugin/Rock5MediaPlayer"
package_tool="$HAIKU_WORK/build/arm64/objects/linux/x86_64/release/tools/package/package"
package="$HAIKU_WORK/rock5-image-extras/packages/rock5_ffmpeg-6.1.6-2-arm64.hpkg"

for input in "$ffmpeg/lib/libavcodec.so.60.31.102" "$plugin" "$mpp_plugin" \
        "$mpp_library" "$player" "$package_tool"; do
    test -s "$input" || { echo "Missing FFmpeg input: $input" >&2; exit 1; }
done

stage=$(mktemp -d "$HAIKU_WORK/tmp/rock5-ffmpeg-stage-XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
mkdir -p "$stage/lib" "$stage/add-ons/media/plugins" "$stage/apps" "$stage/data/licenses"
cp -a "$ffmpeg/lib"/lib{avcodec,avfilter,avformat,avutil,swresample,swscale}.so* "$stage/lib/"
cp "$plugin" "$stage/add-ons/media/plugins/ffmpeg"
cp "$mpp_plugin" "$stage/add-ons/media/plugins/00_rockchip_mpp"
cp "$mpp_library" "$stage/lib/librockchip_mpp.so.1"
cp "$player" "$stage/apps/Rock5MediaPlayer"
cp "$HAIKU_WORK/build/rock5-ffmpeg-arm64/ffmpeg-6.1.6/COPYING.LGPLv2.1" \
    "$stage/data/licenses/LGPLv2.1"
cp "$HAIKU_SOURCE/data/system/data/licenses/MIT" "$stage/data/licenses/MIT"
cp "$HAIKU_SOURCE/data/system/data/licenses/Apache v2" \
    "$stage/data/licenses/Apache v2"
"${cross}strip" --strip-debug "$stage/add-ons/media/plugins/ffmpeg"
"${cross}strip" --strip-debug "$stage/add-ons/media/plugins/00_rockchip_mpp"
"${cross}strip" --strip-debug "$stage/apps/Rock5MediaPlayer"
cat > "$stage/.PackageInfo" <<'EOF'
name rock5_ffmpeg
version 6.1.6-2
architecture arm64
summary "FFmpeg codecs and Media Kit plugin for ROCK 5 ITX"
description "FFmpeg 6.1.6 shared libraries, Rockchip MPP hardware video decoding, Media Kit plugins, and a sample movie player for arm64."
packager "jmgasper"
vendor "jmgasper"
copyrights { "2000-2024 FFmpeg developers" "2015-2026 Rockchip Electronics Co. LTD" "2004-2026 Haiku, Inc." }
licenses { "LGPLv2.1" "Apache v2" "MIT" }
provides {
    rock5_ffmpeg = 6.1.6
}
requires {
    haiku >= r1~beta6
}
EOF
rm -f "$package"
"$package_tool" create -C "$stage" "$package"
"$package_tool" list -p "$package"
sha256sum "$package"
