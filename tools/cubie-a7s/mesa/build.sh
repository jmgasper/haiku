#!/usr/bin/env bash
# Build Mesa's PowerVR Vulkan driver (libvulkan_powervr_mesa.so) and its
# test programs for air/OS arm64 (Cubie A7S, PowerVR BXM-4-64 MC1).
#
#   build.sh [all|fetch|host|patch|configure|driver|tests|shim]   (default: all)
#
# From a clean $ROOT, "all" downloads and verifies the pinned Mesa source,
# builds Mesa's host shader tools (mesa_clc, vtn_bindgen2, pco_clc) at the
# same version, applies mesa-haiku-pvr.patch, cross-builds the driver and
# the three test programs and copies the results to $ROOT/out. Every step
# is incremental; rerunning it is cheap.
#
# The driver talks to the air/OS kernel driver "powervr" through
# /dev/graphics/powervr/0; its userland contract is pvr_haiku.h in this
# tree (headers/private/graphics/powervr), used by path, not copied.
set -euo pipefail

MESA_VERSION=26.2.4
MESA_ARCHIVE=mesa-$MESA_VERSION.tar.xz
MESA_URL=https://archive.mesa3d.org/$MESA_ARCHIVE
# docs/relnotes/26.2.4.rst ("SHA checksums"); the archive's .sig is by Eric
# Engestrom, 57551DE15B968F6341C248F68D8E31AFC32428A6
MESA_SHA256=bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9

WORK=/mnt/HaikuWork
ROOT=${CUBIE_MESA_ROOT:-$WORK/cubie/mesa}
# the ROCK 5's pinned arm64 Haiku sysroot and cross file (also used by rpi4)
BASE=${CUBIE_MESA_BASE:-$WORK/artifacts/mali-system-opengl-build/20260918T125722Z}
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TREE=$(cd "$TOOLS/../../.." && pwd)
PVR_HEADERS=${PVR_HAIKU_HEADERS:-$TREE/headers/private/graphics/powervr}
CROSS=$WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku
NATIVE_DEPS=$WORK/toolchains/mesa-native-deps
JOBS=${HAIKU_JOBS:-16}

SRC=$ROOT/mesa-$MESA_VERSION
HOST_BUILD=$ROOT/build-host
HOST_TOOLS=$ROOT/host-tools
BUILD=$ROOT/build
OUT=$ROOT/out
PATCH=$TOOLS/mesa-haiku-pvr.patch
TESTS=(pvr_vkprobe pvr_vkfill pvr_vkfence pvr_vktriangle)

export PATH=$HOST_TOOLS/bin:$WORK/toolchains/mesa-python/bin:$WORK/toolchains/host/usr/bin:$PATH
export LD_LIBRARY_PATH=$NATIVE_DEPS/usr/lib/x86_64-linux-gnu:$NATIVE_DEPS/usr/lib/llvm-18/lib
export TMPDIR=$WORK/tmp
unset PKG_CONFIG_PATH
mkdir -p "$ROOT" "$TMPDIR"

log() { printf '== %s\n' "$*"; }

# -- source -------------------------------------------------------------------
fetch() {
	mkdir -p "$ROOT/downloads"
	local archive=$ROOT/downloads/$MESA_ARCHIVE
	if [ ! -f "$archive" ]; then
		log "downloading $MESA_URL"
		curl -fL --retry 3 -o "$archive.part" "$MESA_URL"
		mv "$archive.part" "$archive"
	fi
	echo "$MESA_SHA256  $archive" | sha256sum -c --quiet - || {
		echo "$archive: sha256 mismatch" >&2; exit 1; }
	if [ ! -f "$SRC/VERSION" ]; then
		log "unpacking $MESA_ARCHIVE"
		rm -rf "$SRC.tmp" && mkdir -p "$SRC.tmp"
		tar -xf "$archive" -C "$SRC.tmp"
		mv "$SRC.tmp/mesa-$MESA_VERSION" "$SRC" && rmdir "$SRC.tmp"
	fi
	[ "$(cat "$SRC/VERSION")" = "$MESA_VERSION" ]
}

# -- the patch ----------------------------------------------------------------
# Applied once; a stamp holds the applied patch's hash. A changed patch is
# re-applied after reverting the old one (kept as $SRC/.haiku-pvr.patch).
patch_source() {
	local want have=
	want=$(sha256sum "$PATCH" | cut -d' ' -f1)
	[ ! -f "$SRC/.haiku-pvr.sha256" ] || have=$(cat "$SRC/.haiku-pvr.sha256")
	[ "$want" != "$have" ] || { log "patch already applied"; return 0; }
	if [ -n "$have" ]; then
		log "reverting the previous patch"
		patch -d "$SRC" -p1 -R -s --no-backup-if-mismatch < "$SRC/.haiku-pvr.patch"
		rm -f "$SRC/.haiku-pvr.sha256"
	fi
	log "applying $(basename "$PATCH")"
	patch -d "$SRC" -p1 --dry-run -s --no-backup-if-mismatch < "$PATCH"
	patch -d "$SRC" -p1 -s --no-backup-if-mismatch < "$PATCH"
	cp "$PATCH" "$SRC/.haiku-pvr.patch"
	echo "$want" > "$SRC/.haiku-pvr.sha256"
}

# -- host tools ---------------------------------------------------------------
# Mesa's precompiled shaders are serialized NIR and USC code, so the build
# tools must be this exact Mesa version: mesa_clc (OpenCL C -> SPIR-V),
# vtn_bindgen2 (SPIR-V -> NIR builder functions) and pco_clc (the PowerVR
# compiler run on the USC library). They are built natively against the
# host's LLVM 18 with clang-cpp, LLVMSPIRVLib and SPIRV-Tools from
# toolchains/mesa-native-deps; toolchains/mesa-host (Mesa 25.3.6) is not
# touched.
host_tools() {
	local stamp=$HOST_TOOLS/.mesa-version
	if [ -x "$HOST_TOOLS/bin/mesa_clc" ] && [ -x "$HOST_TOOLS/bin/vtn_bindgen2" ] &&
	   [ -x "$HOST_TOOLS/bin/pco_clc" ] && [ "$(cat "$stamp" 2>/dev/null)" = "$MESA_VERSION" ]; then
		log "host tools present ($HOST_TOOLS/bin)"
		return 0
	fi
	log "building host tools"
	local pc=$ROOT/host-pkgconfig nd=$NATIVE_DEPS/usr
	mkdir -p "$pc"
	# the extracted .deb pkg-config files say prefix=/usr
	cat > "$pc/SPIRV-Tools.pc" <<PC
libdir=$nd/lib/x86_64-linux-gnu
includedir=$nd/include
Name: SPIRV-Tools
Description: Tools for SPIR-V (toolchains/mesa-native-deps)
Version: 2025.1.1
Libs: -L\${libdir} -lSPIRV-Tools-opt -lSPIRV-Tools -lSPIRV-Tools-link
Cflags: -I\${includedir}
PC
	cat > "$pc/LLVMSPIRVLib.pc" <<PC
libdir=$nd/lib/x86_64-linux-gnu
includedir=$nd/include
Name: LLVMSPIRVLib
Description: LLVM/SPIR-V bi-directional translator (toolchains/mesa-native-deps)
Version: 18.1.0.0
Libs: -L\${libdir} -lLLVMSPIRVLib
Cflags: -I\${includedir}
PC
	# clang-cpp's development link lives only in mesa-native-deps; the
	# run-time library is the host's (identical) one
	cat > "$ROOT/host-native.ini" <<INI
[binaries]
llvm-config = '/usr/bin/llvm-config-18'

[properties]
pkg_config_libdir = ['$pc', '/usr/lib/x86_64-linux-gnu/pkgconfig', '/usr/share/pkgconfig']

[built-in options]
c_link_args = ['-L$nd/lib/llvm-18/lib', '-Wl,-rpath,$nd/lib/x86_64-linux-gnu']
cpp_link_args = ['-L$nd/lib/llvm-18/lib', '-Wl,-rpath,$nd/lib/x86_64-linux-gnu']
INI
	if [ ! -f "$HOST_BUILD/build.ninja" ]; then
		LIBRARY_PATH=$nd/lib/llvm-18/lib meson setup "$HOST_BUILD" "$SRC" \
			--native-file="$ROOT/host-native.ini" --prefix="$HOST_TOOLS" \
			--buildtype=release --wrap-mode=nofallback \
			-Dplatforms= -Dgallium-drivers= -Dvulkan-drivers=imagination \
			-Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled \
			-Dglx=disabled -Dgbm=disabled -Dglvnd=disabled -Dllvm=enabled \
			-Dshared-llvm=enabled -Dmesa-clc=enabled -Dinstall-mesa-clc=true \
			-Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true \
			-Dspirv-tools=enabled -Dbuild-tests=false '-Dtools=[]' \
			-Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
			-Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=disabled \
			-Dvulkan-layers=[] -Dintel-rt=disabled
	fi
	LIBRARY_PATH=$nd/lib/llvm-18/lib ninja -C "$HOST_BUILD" -j"$JOBS" \
		src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 \
		src/imagination/pco/uscgen/pco_clc
	mkdir -p "$HOST_TOOLS/bin"
	install -m755 "$HOST_BUILD/src/compiler/clc/mesa_clc" \
		"$HOST_BUILD/src/compiler/spirv/vtn_bindgen2" \
		"$HOST_BUILD/src/imagination/pco/uscgen/pco_clc" "$HOST_TOOLS/bin/"
	echo "$MESA_VERSION" > "$stamp"
}

# -- the driver ---------------------------------------------------------------
# Only the PowerVR Vulkan driver: no GL, no EGL, no LLVM (the shader
# compilers that need it ran on the host), no window system integration.
configure() {
	[ -f "$PVR_HEADERS/pvr_haiku.h" ] || {
		echo "pvr_haiku.h not found in $PVR_HEADERS" >&2; exit 1; }
	[ -f "$BASE/haiku-aarch64.ini" ] || {
		echo "no Haiku arm64 sysroot/cross file at $BASE" >&2; exit 1; }
	local sysroot=$BASE/sysroot
	# BASE's cross file names the compilers and the sysroot; this one adds
	# the kernel driver's userland header directory to every compile
	cat > "$ROOT/haiku-aarch64-pvr.ini" <<INI
[built-in options]
c_args = ['--sysroot=$sysroot', '-I$PVR_HEADERS']
cpp_args = ['--sysroot=$sysroot', '-I$PVR_HEADERS']
c_link_args = ['--sysroot=$sysroot']
cpp_link_args = ['--sysroot=$sysroot']
INI
	local reconfigure=()
	[ ! -f "$BUILD/build.ninja" ] || reconfigure=(--reconfigure)
	meson setup "${reconfigure[@]}" "$BUILD" "$SRC" \
		--cross-file="$BASE/haiku-aarch64.ini" \
		--cross-file="$ROOT/haiku-aarch64-pvr.ini" \
		--prefix=/boot/system/non-packaged --libdir=lib \
		--buildtype=debugoptimized --wrap-mode=nofallback \
		-Dplatforms=haiku -Dvulkan-drivers=imagination '-Dgallium-drivers=[]' \
		-Dimagination-srv=false -Dopengl=false -Dgles1=disabled \
		-Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
		-Dglvnd=disabled -Dllvm=disabled -Dmesa-clc=system \
		-Dprecomp-compiler=system -Dspirv-tools=disabled \
		-Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
		-Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled \
		-Dbuild-tests=false '-Dtools=[]' '-Dvulkan-layers=[]'
}

driver() {
	[ -f "$BUILD/build.ninja" ] || configure
	ninja -C "$BUILD" -j"$JOBS" src/imagination/vulkan/libvulkan_powervr_mesa.so \
		src/imagination/vulkan/powervr_mesa_icd.aarch64.json
}

# -- tests and results --------------------------------------------------------
# The test programs link the driver directly: there is no Vulkan loader on
# arm64 Haiku. They start from vk_icdGetInstanceProcAddr.
# Linking them also checks that every symbol the driver needs is in the
# sysroot's libroot/libstdc++ (--no-allow-shlib-undefined).
tests() {
	local lib=$BUILD/src/imagination/vulkan
	mkdir -p "$ROOT/tests"
	for t in "${TESTS[@]}"; do
		"$CROSS-gcc" --sysroot="$BASE/sysroot" -std=gnu11 -O2 -g -Wall \
			-Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
			-I"$SRC/include" -o "$ROOT/tests/$t" "$TOOLS/$t.c" \
			-L"$lib" -lvulkan_powervr_mesa -Wl,--no-allow-shlib-undefined \
			-Wl,-rpath-link="$BASE/sysroot/boot/system/lib"
	done
}

results() {
	local lib=$BUILD/src/imagination/vulkan
	mkdir -p "$OUT/debug"
	cp "$lib/libvulkan_powervr_mesa.so" "$OUT/debug/"
	"$CROSS-strip" -o "$OUT/libvulkan_powervr_mesa.so" \
		"$lib/libvulkan_powervr_mesa.so"
	for t in "${TESTS[@]}"; do
		cp "$ROOT/tests/$t" "$OUT/debug/"
		"$CROSS-strip" -o "$OUT/$t" "$ROOT/tests/$t"
	done
	# for a Vulkan loader later (library in /boot/system/non-packaged/lib);
	# the tests do not read it
	cp "$lib/powervr_mesa_icd.aarch64.json" "$OUT/"
	{
		echo "mesa $MESA_VERSION sha256 $MESA_SHA256"
		echo "patch $(sha256sum "$PATCH" | cut -d' ' -f1) $(basename "$PATCH")"
		echo "pvr_haiku.h $(sha256sum "$PVR_HEADERS/pvr_haiku.h" | cut -d' ' -f1) $PVR_HEADERS/pvr_haiku.h"
		echo "sysroot $BASE"
		echo "host tools $(cat "$HOST_TOOLS/.mesa-version") $HOST_TOOLS/bin"
		(cd "$OUT" && sha256sum libvulkan_powervr_mesa.so "${TESTS[@]}")
	} > "$OUT/MANIFEST"
	log "results in $OUT"
	ls -l "$OUT"
}

# -- host smoke test (optional, not part of "all") ----------------------------
# The same patched source built for Linux (the Haiku hunks compile out) with
# Mesa's pvr drm-shim, which answers DEV_QUERY like the Linux kernel driver
# for any BVNC. The three tests run on the build host against BXM-4-64
# 36.56.104.183 under an ioctl tracer: the driver's own code (enumeration,
# device creation, the PowerVR shader compiler, command streams, null jobs)
# runs, and every ioctl it makes is logged to $ROOT/shim/<test>.log, in
# order, with its flags. Nothing executes: the shim ignores SUBMIT_JOBS and
# its syncobj ioctls succeed at once, so pvr_vkfill finds every word
# unwritten and pvr_vkfence's waits that must time out do not.
SHIM_BVNC=36.56.104.183
shim() {
	local sb=$ROOT/build-shim
	if [ ! -f "$sb/build.ninja" ]; then
		meson setup "$sb" "$SRC" --buildtype=debugoptimized \
			--wrap-mode=nofallback -Dplatforms= '-Dgallium-drivers=[]' \
			-Dvulkan-drivers=imagination -Dtools=drm-shim -Dopengl=false \
			-Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled \
			-Dgbm=disabled -Dglvnd=disabled -Dllvm=disabled -Dmesa-clc=system \
			-Dprecomp-compiler=system -Dspirv-tools=disabled \
			-Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
			-Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled \
			-Dbuild-tests=false '-Dvulkan-layers=[]'
	fi
	ninja -C "$sb" -j"$JOBS" src/imagination/vulkan/libvulkan_powervr_mesa.so \
		src/imagination/drm-shim/libpowervr_noop_drm_shim.so
	local lib=$sb/src/imagination/vulkan s=$ROOT/shim
	mkdir -p "$s"
	cc -shared -fPIC -O2 -Wall -I"$SRC/include" -o "$s/libpvr_ioctl_trace.so" \
		"$TOOLS/pvr_ioctl_trace.c" -ldl -lpthread
	local preload=$s/libpvr_ioctl_trace.so:$sb/src/imagination/drm-shim/libpowervr_noop_drm_shim.so
	for t in "${TESTS[@]}"; do
		cc -std=gnu11 -O2 -g -Wall -I"$SRC/include" -o "$s/$t" "$TOOLS/$t.c" \
			-L"$lib" -lvulkan_powervr_mesa -Wl,-rpath,"$lib"
		PVR_SHIM_DEVICE_BVNC=$SHIM_BVNC PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
			LD_PRELOAD=$preload timeout 120 "$s/$t" > "$s/$t.log" 2>&1 || true
		printf '%-12s %5s ioctls, last line: %s\n' "$t" \
			"$(grep -c '^ioctl ' "$s/$t.log")" "$(tail -1 "$s/$t.log")"
	done
	log "logs in $s"
}

case "${1:-all}" in
shim) fetch; patch_source; host_tools; shim ;;
fetch) fetch ;;
host) fetch; patch_source; host_tools ;;
patch) fetch; patch_source ;;
configure) fetch; patch_source; host_tools; configure ;;
driver) fetch; patch_source; host_tools; driver ;;
tests) tests ;;
all) fetch; patch_source; host_tools; configure; driver; tests; results ;;
*) echo "usage: $0 [all|fetch|host|patch|configure|driver|tests|shim]" >&2; exit 2 ;;
esac
