#!/bin/bash
# Build the Zink OpenGL renderer add-on on the workstation and install it.
#
# Haiku's OpenGL kit (libGL.so and the renderer add-ons) is part of the mesa
# package, which is 22.0.5 on this machine, so the add-on is built from the
# same Mesa release: that keeps the glapi dispatch between libGL and the add-on
# identical. Zink reaches the GPU through the Vulkan loader, which finds NVK.
#
# usage: build-mesa-hgl.sh [--sync]     (--sync copies the source tree first)
set -euo pipefail
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=10 ws-haiku"
SRC=/mnt/HaikuWork/x399/src/mesa-build/mesa-hgl
REMOTE=/boot/home/build/src/mesa-hgl

if [ "${1:-}" = "--sync" ]; then
	# -m stamps the extracted files with the workstation's own clock: it runs a
	# few minutes ahead of this machine, and ninja would otherwise decide that
	# freshly copied sources are older than the objects built from the last
	# ones and rebuild nothing.
	tar -C "$SRC" -czf - --exclude=.git --exclude=build . \
		| $SSH "mkdir -p $REMOTE && cd $REMOTE && tar -xmzf -"
fi

$SSH "set -e
	cd $REMOTE
	export PATH=/boot/home/build/pybin:\$PATH
	if [ ! -e build/build.ninja ]; then
		env PYTHONPATH=/boot/system/lib/python3.14/vendor-packages python3.10 \
			/boot/system/bin/meson setup build \
			-Dplatforms=haiku -Dgallium-drivers=zink,swrast \
			-Dglx=disabled -Degl=disabled -Dllvm=disabled -Dvulkan-drivers= \
			-Dshared-glapi=enabled -Dgbm=disabled -Dgles1=disabled \
			-Dgles2=disabled -Dosmesa=false --buildtype=release \
			--prefix=/boot/home/build/install-hgl
	fi
	ninja -C build -j28
	mkdir -p /boot/home/config/non-packaged/add-ons/opengl
	cp build/src/gallium/targets/haiku-zink/libzinkpipe.so \
		'/boot/home/config/non-packaged/add-ons/opengl/Zink'
	echo 'installed the Zink renderer'"
