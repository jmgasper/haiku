#!/bin/bash
# Build one of tests/*.c against the NVDEC decoder, on the workstation.
# Usage: build-nvdec.sh <name> [extra gcc arguments...]
set -eu
X399=${X399:-/mnt/HaikuWork/x399}
HAIKU=$X399/haiku
PLUGIN=src/add-ons/media/plugins/nvdec
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
name=$1; shift
$SSH 'mkdir -p /boot/home/build/nvdec'
scp -F $X399/ssh/config -q $X399/tests/$name.c $HAIKU/$PLUGIN/* ws-haiku:/boot/home/build/nvdec/
$SSH "cd /boot/home/build/nvdec \
	&& NVRM=/boot/home/build/src/mesa-nvk/src/nouveau/vulkan/nvkmd/nvrm \
	&& O=\$NVRM/open-gpu-kernel-modules \
	&& gcc -O2 -g -o $name $name.c nvdec_engine.c nvdec_h264.c h264_parse.c \
		\$NVRM/nvRmApi.c \
		-I. -I\$NVRM \
		-I\$O/src/common/sdk/nvidia/inc \
		-I\$O/src/nvidia/arch/nvalloc/common/inc \
		-I\$O/src/nvidia/arch/nvalloc/unix/include \
		-I\$O/src/nvidia/inc/kernel \
		-I/boot/home/build/src/mesa-nvk/src/nouveau/headers/nvidia \
		$*"
