#!/usr/bin/env bash
# Configure and build Mesa's v3d driver for air/OS (Raspberry Pi 4).
#
# This sits on top of the ROCK 5's pinned Haiku Mesa port: it uses that
# build's sysroot, cross file and libglvnd (BASE), and a copy of its patched
# Mesa 25.3.6 source with the v3d patch of this directory applied on top.
#   build.sh [configure|build]     (default: both, configuring only once)
set -euo pipefail
BASE=${RPI4_MESA_BASE:-/mnt/HaikuWork/artifacts/mali-system-opengl-build/20260918T125722Z}
ROOT=${RPI4_MESA_ROOT:-/mnt/HaikuWork/rpi4/mesa}
WORK=/mnt/HaikuWork
export PATH=$WORK/toolchains/mesa-python/bin:$WORK/toolchains/mesa-host/bin:$WORK/toolchains/host/usr/bin:$PATH
export LD_LIBRARY_PATH=$WORK/toolchains/mesa-native-deps/usr/lib/x86_64-linux-gnu:$WORK/toolchains/mesa-native-deps/usr/lib/llvm-18/lib
export TMPDIR=$WORK/tmp
unset PKG_CONFIG_PATH

mkdir -p "$ROOT"
[ -d "$ROOT/mesa-25.3.6" ] || cp -a "$BASE/mesa-25.3.6" "$ROOT/"

CONFIGURE=${1:-}
if [ "$CONFIGURE" = configure ] || [ ! -f "$ROOT/build/build.ninja" ]; then
    rm -rf "$ROOT/build"
    meson setup "$ROOT/build" "$ROOT/mesa-25.3.6" \
        --cross-file="$BASE/haiku-aarch64.ini" --prefix="$ROOT/install" \
        --libdir=lib --buildtype=debugoptimized --wrap-mode=nofallback \
        -Dplatforms=haiku -Dexpat=disabled -Dgallium-drivers=v3d,softpipe \
        -Dgallium-va=disabled '-Dvulkan-drivers=[]' -Dshader-cache=disabled \
        -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dgbm=disabled \
        -Dglx=disabled -Degl=enabled -Dglvnd=enabled -Dllvm=disabled \
        -Dvalgrind=disabled -Dbuild-tests=false '-Dtools=[]' -Dzstd=disabled \
        -Dzlib=disabled -Dxmlconfig=disabled -Dmesa-clc=system \
        -Dprecomp-compiler=system -Dspirv-tools=disabled
fi
[ "$CONFIGURE" = configure ] || ninja -C "$ROOT/build" -j"${HAIKU_JOBS:-12}"

# The Vulkan driver (v3dv) is a build of its own: libvulkan_broadcom.so.
# There is no Vulkan loader for arm64 Haiku and no window system layer; a
# program links the library and starts from vk_icdGetInstanceProcAddr (see
# vk_probe.c).
if [ "$CONFIGURE" = configure ] || [ ! -f "$ROOT/build-vk/build.ninja" ]; then
    rm -rf "$ROOT/build-vk"
    meson setup "$ROOT/build-vk" "$ROOT/mesa-25.3.6" \
        --cross-file="$BASE/haiku-aarch64.ini" --prefix="$ROOT/install-vk" \
        --libdir=lib --buildtype=debugoptimized --wrap-mode=nofallback \
        -Dplatforms=haiku -Dexpat=disabled '-Dgallium-drivers=[]' \
        -Dgallium-va=disabled -Dvulkan-drivers=broadcom \
        -Dshader-cache=disabled -Dgles1=disabled -Dgles2=disabled \
        -Dopengl=false -Dgbm=disabled -Dglx=disabled -Degl=disabled \
        -Dglvnd=disabled -Dllvm=disabled -Dvalgrind=disabled \
        -Dbuild-tests=false '-Dtools=[]' -Dzstd=disabled -Dzlib=disabled \
        -Dxmlconfig=disabled -Dmesa-clc=system -Dprecomp-compiler=system \
        -Dspirv-tools=disabled
fi
[ "$CONFIGURE" = configure ] && exit 0
ninja -C "$ROOT/build-vk" -j"${HAIKU_JOBS:-12}"

# What the image build takes (tools/rpi4/UserBuildConfig): the stripped
# library and the EGL vendor file that names it.
mkdir -p "$ROOT/stage"
"$WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-strip" \
    -o "$ROOT/stage/libEGL_mesa.so.0" "$ROOT/build/src/egl/libEGL_mesa.so.0.0.0"
"$WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-strip" \
    -o "$ROOT/stage/libvulkan_broadcom.so" \
    "$ROOT/build-vk/src/broadcom/vulkan/libvulkan_broadcom.so"
cat > "$ROOT/stage/10_mesa.json" <<'JSON'
{
  "file_format_version": "1.0.0",
  "ICD": {
    "library_path": "/boot/system/non-packaged/lib/libEGL_mesa.so.0"
  }
}
JSON
