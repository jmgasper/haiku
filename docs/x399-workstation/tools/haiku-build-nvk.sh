#!/bin/bash
# Native build of NVK (Vulkan) for nvidia_rm on the X399 workstation.
# Runs on Haiku. Installs into /boot/home/build/install.
set -euo pipefail
B=/boot/home/build
SRC=$B/src
PREFIX=$B/install
JOBS=${JOBS:-28}
export PATH=$PREFIX/bin:$PATH
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig:$PREFIX/develop/lib/pkgconfig:${PKG_CONFIG_PATH:-}
mkdir -p $B/pybin && ln -sf /boot/system/bin/python3.10 $B/pybin/python3
export PATH=$B/pybin:$PATH
MESON="env PYTHONPATH=/boot/system/lib/python3.14/vendor-packages python3.10 /boot/system/bin/meson"
mkdir -p $PREFIX $B/obj
step() { echo "=== $(date +%T) $*"; }

if [ ! -e $PREFIX/bin/llvm-spirv ]; then
	step SPIRV-LLVM-Translator
	cmake -S $SRC/SPIRV-LLVM-Translator -B $B/obj/spirv-llvm -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX \
		-DLLVM_DIR=$(llvm-config --cmakedir) \
		-DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR=$SRC/SPIRV-Headers \
		-DLLVM_SPIRV_INCLUDE_TESTS=OFF -DBUILD_SHARED_LIBS=OFF
	ninja -C $B/obj/spirv-llvm -j$JOBS install
fi

if [ ! -e $PREFIX/share/pkgconfig/libclc.pc ]; then
	step libclc
	mkdir -p $B/obj/llvm-src && cd $B/obj/llvm-src
	[ -d libclc ] || { tar -xJf $SRC/libclc-20.1.8.src.tar.xz && mv libclc-20.1.8.src libclc; }
	[ -d cmake ] || { tar -xJf $SRC/cmake-20.1.8.src.tar.xz && mv cmake-20.1.8.src cmake; }
	cmake -S $B/obj/llvm-src/libclc -B $B/obj/libclc -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX \
		-DLLVM_DIR=$(llvm-config --cmakedir) \
		-DLLVM_SPIRV=$PREFIX/bin/llvm-spirv \
		-DLIBCLC_TARGETS_TO_BUILD="spirv-mesa3d-;spirv64-mesa3d-"
	ninja -C $B/obj/libclc -j$JOBS install
fi

step mesa-nvk
cd $SRC/mesa-nvk
[ -e $B/obj/mesa-nvk/build.ninja ] || $MESON setup $B/obj/mesa-nvk \
	-Dprefix=$PREFIX -Dbuildtype=release \
	-Dgallium-drivers= -Dvulkan-drivers=nouveau -Dplatforms= \
	-Dgallium-rusticl=false -Degl=disabled -Dglvnd=disabled -Dglx=disabled \
	-Ddisplay-info=disabled -Dllvm=enabled -Dzstd=enabled
ninja -C $B/obj/mesa-nvk -j$JOBS install

# Put the driver where the Vulkan loader looks, so that every program finds
# the GPU without anything being set in its environment. The loader searches
# the add-ons directories, not the data ones - ask it with VK_LOADER_DEBUG=all
# if this ever stops working - and the manifest names the driver by absolute
# path, so nothing needs LD_LIBRARY_PATH either.
ICD=/boot/system/non-packaged/add-ons/vulkan/icd.d
mkdir -p $ICD
cp $PREFIX/data/vulkan/icd.d/nouveau_icd.x86_64.json $ICD/
echo "installed the driver manifest into $ICD"

step done
