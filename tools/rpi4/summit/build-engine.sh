#!/usr/bin/env bash
# Rebuild the configured Pi WebRTC engine, preserving its feature options.
# The earlier summit-gl build lacks features shipped in the release engine.
set -euo pipefail
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$TOOLS/../../rock5-itx/env.sh"
SOURCE=${SUMMIT_WEBKIT_SOURCE:-/mnt/HaikuWork/build/summit-arm64/WebKit}
BUILD=${SUMMIT_ENGINE:-/mnt/HaikuWork/rpi4/summit-rtc/WebKitBuild}
LOG=${SUMMIT_ENGINE_LOG:-/mnt/HaikuWork/rpi4/summit-rtc/performance-build.log}
REVISION=${SUMMIT_WEBKIT_REVISION:-6c00cc5d517150348a6b4872ab9fdcc345d389b6}
[[ -f $BUILD/CMakeCache.txt ]] || { echo "Configure the WebRTC engine first: $BUILD" >&2; exit 1; }
[[ $(git -C "$SOURCE" rev-parse HEAD) == "$REVISION" ]] || {
    echo "Expected the explicitly pinned WebKit snapshot $REVISION" >&2; exit 1;
}
for option in ENABLE_WEBGL ENABLE_WEB_RTC USE_HAIKU_GL_COMPOSITING; do
    grep -q "^$option:BOOL=ON$" "$BUILD/CMakeCache.txt" || {
        echo "Required release feature $option is disabled in $BUILD" >&2; exit 1;
    }
done
python3 "$TOOLS/../mesa/apply-patches.py" "$SOURCE" \
    "$TOOLS/webkit-haiku-exports.patch" "$TOOLS/webkit-haiku-readback-hint.patch" \
    "$TOOLS/webkit-shared-buffer-growth.patch"
source /mnt/HaikuWork/build/summit-arm64/hosttools/env.sh
# Runtime dependencies must come from the packaged engine, not the build tree
# or older system libraries with the same soname.
cmake -S "$SOURCE" -B "$BUILD" -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    '-DCMAKE_INSTALL_RPATH=$ORIGIN;$ORIGIN/lib' > "$LOG" 2>&1
ninja -C "$BUILD" -j"${HAIKU_JOBS:-8}" WebKit WebProcess NetworkProcess >> "$LOG" 2>&1
# The app packager uses this completion marker to reject incomplete builds.
echo 'ninja exit 0' >> "$LOG"
echo "Built $BUILD; log $LOG"
