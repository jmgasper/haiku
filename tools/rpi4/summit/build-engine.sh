#!/usr/bin/env bash
# Rebuild the configured Pi WebRTC engine, preserving its feature options.
# The earlier summit-gl build lacks features shipped in the release engine.
set -euo pipefail
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$TOOLS/../../rock5-itx/env.sh"
SOURCE=${SUMMIT_WEBKIT_SOURCE:-/mnt/HaikuWork/build/summit-arm64/WebKit}
BUILD=${SUMMIT_ENGINE:-/mnt/HaikuWork/rpi4/summit-rtc/WebKitBuild}
LOG=${SUMMIT_ENGINE_LOG:-/mnt/HaikuWork/rpi4/summit-rtc/performance-build.log}
[[ -f $BUILD/CMakeCache.txt ]] || { echo "Configure the WebRTC engine first: $BUILD" >&2; exit 1; }
[[ $(git -C "$SOURCE" rev-parse HEAD) == 6c00cc5d517150348a6b4872ab9fdcc345d389b6 ]] || {
    echo "Expected the lab's 6c00cc5d snapshot (WebKit 00991b6c plus Haiku port)" >&2; exit 1;
}
for option in ENABLE_WEBGL ENABLE_WEB_RTC USE_HAIKU_GL_COMPOSITING; do
    grep -q "^$option:BOOL=ON$" "$BUILD/CMakeCache.txt" || {
        echo "Required release feature $option is disabled in $BUILD" >&2; exit 1;
    }
done
PATCH=$TOOLS/webkit-haiku-exports.patch
if patch --batch --dry-run -N -p1 -d "$SOURCE" < "$PATCH" >/dev/null 2>&1; then
    patch --batch -N -p1 -d "$SOURCE" < "$PATCH"
elif ! patch --batch --dry-run -R -p1 -d "$SOURCE" < "$PATCH" >/dev/null 2>&1; then
    echo "Cannot apply or recognize the export patch; inspect the engine source" >&2; exit 1
fi
source /mnt/HaikuWork/build/summit-arm64/hosttools/env.sh
cmake -S "$SOURCE" -B "$BUILD" > "$LOG" 2>&1
ninja -C "$BUILD" -j"${HAIKU_JOBS:-8}" WebKit WebProcess NetworkProcess >> "$LOG" 2>&1
# The app packager uses this completion marker to reject incomplete builds.
echo 'ninja exit 0' >> "$LOG"
echo "Built $BUILD; log $LOG"
