#!/bin/bash
# Build the NVDEC media add-on on the workstation and install it for the user.
#
# It is built there rather than with the rest of the tree because it needs
# NVIDIA's resource manager headers, which live in the NVK checkout on the
# machine and are not part of this repository.
set -eu
X399=${X399:-/mnt/HaikuWork/x399}
HAIKU=$X399/haiku
PLUGIN=src/add-ons/media/plugins/nvdec
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"

$SSH 'mkdir -p /boot/home/build/nvdec'
scp -F $X399/ssh/config -q $HAIKU/$PLUGIN/* ws-haiku:/boot/home/build/nvdec/
$SSH "cd /boot/home/build/nvdec \
	&& NVRM=/boot/home/build/src/mesa-nvk/src/nouveau/vulkan/nvkmd/nvrm \
	&& O=\$NVRM/open-gpu-kernel-modules \
	&& INC=\"-I. -I\$NVRM \
		-I\$O/src/common/sdk/nvidia/inc \
		-I\$O/src/nvidia/arch/nvalloc/common/inc \
		-I\$O/src/nvidia/arch/nvalloc/unix/include \
		-I\$O/src/nvidia/inc/kernel \
		-I/boot/home/build/src/mesa-nvk/src/nouveau/headers/nvidia \
		-I/boot/system/develop/headers/private/media\" \
	&& for f in nvdec_engine nvdec_h264 h264_parse nvdec_convert; do \
		gcc -O2 -g -fPIC -c -o \$f.o \$f.c \$INC || exit 1; \
	done \
	&& gcc -O2 -g -fPIC -c -o nvRmApi.o \$NVRM/nvRmApi.c \$INC \
	&& g++ -O2 -g -fPIC -c -o NVDecPlugin.o NVDecPlugin.cpp \$INC \
	&& g++ -shared -o nvdec NVDecPlugin.o nvdec_engine.o nvdec_h264.o \
		h264_parse.o nvdec_convert.o nvRmApi.o -lbe -lmedia \
	&& mkdir -p /boot/home/config/non-packaged/add-ons/media/plugins \
	&& cp nvdec /boot/home/config/non-packaged/add-ons/media/plugins/nvdec \
	&& echo installed /boot/home/config/non-packaged/add-ons/media/plugins/nvdec"
