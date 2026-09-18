#!/bin/bash
# Build and run the GPU benchmark on the workstation.
#
# usage: build-vkbench.sh [benchmark...]      (compute, fill, copy, upload)
set -euo pipefail
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=10 ws-haiku"
SRC=/mnt/HaikuWork/x399/tests

tar -C "$SRC" -cf - vkbench.c vk_haiku_scanout.h shaders | $SSH 'mkdir -p /boot/home/tests && cd /boot/home/tests && tar -xmf -'
$SSH 'set -e
	cd /boot/home/tests
	for s in shaders/*.vert shaders/*.frag shaders/*.comp; do
		glslangValidator -V "$s" -o "$s.spv" >/dev/null
	done
	gcc -O2 -o vkbench vkbench.c -lvulkan'
$SSH "cd /boot/home/tests && VK_ICD_FILENAMES=/boot/home/build/install/data/vulkan/icd.d/nouveau_icd.x86_64.json LD_LIBRARY_PATH=/boot/home/build/install/lib ./vkbench $*"
