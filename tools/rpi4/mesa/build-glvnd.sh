#!/usr/bin/env bash
# Build the Pi's BGLView library from the same pinned Haiku libglvnd port.
set -euo pipefail
BASE=${RPI4_MESA_BASE:-/mnt/HaikuWork/artifacts/mali-system-opengl-build/20260918T125722Z}
ROOT=${RPI4_GLVND_ROOT:-/mnt/HaikuWork/rpi4/glvnd}
WORK=/mnt/HaikuWork
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export PATH=$WORK/toolchains/mesa-python/bin:$WORK/toolchains/host/usr/bin:$PATH
export TMPDIR=$WORK/tmp XDG_CACHE_HOME=$WORK/cache
export PYTHONPYCACHEPREFIX=$WORK/cache/pycache
unset PKG_CONFIG_PATH

mkdir -p "$ROOT"
[ -d "$ROOT/libglvnd-v1.7.0" ] || cp -a "$BASE/libglvnd-v1.7.0" "$ROOT/"
# The pinned snapshot predates redraw coalescing. Keep that fix when adding
# the background change; never replace or modify the shared ROCK library.
python3 "$TOOLS/apply-patches.py" "$ROOT/libglvnd-v1.7.0" \
    "$TOOLS/../../rock5-itx/mesa/libglvnd-haiku-present.patch" \
    "$TOOLS/libglvnd-haiku-view-background.patch"

reconfigure=()
[ ! -f "$ROOT/build/build.ninja" ] || reconfigure=(--reconfigure)
meson setup "${reconfigure[@]}" "$ROOT/build" "$ROOT/libglvnd-v1.7.0" \
    --cross-file="$BASE/haiku-aarch64.ini" --buildtype=debugoptimized \
    --prefix=/boot/system --libdir=lib --includedir=develop/headers/os/opengl \
    --sysconfdir=settings --wrap-mode=nofallback -Dx11=disabled -Dglx=disabled \
    -Dhgl=true -Dgles1=false -Dgles2=true -Degl=true
ninja -C "$ROOT/build" -j"${HAIKU_JOBS:-8}"
mkdir -p "$ROOT/stage"
"$WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-strip" \
    -o "$ROOT/stage/libGL.so.1" "$ROOT/build/src/HGL/libGL.so.1.0.0"
