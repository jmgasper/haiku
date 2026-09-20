#!/bin/bash
# Build one of tests/*.c against resman's user interface, on the workstation.
# Usage: build-nvtest.sh <name> [extra gcc arguments...]
# The sources come from this directory's tests/, the RM headers from the NVK
# tree already checked out on the machine.
set -eu
X399=${X399:-/mnt/HaikuWork/x399}
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
name=$1; shift
scp -F $X399/ssh/config -q $X399/tests/$name.c $X399/tests/nvdecrm.h ws-haiku:/boot/home/build/tests/
$SSH "cd /boot/home/build/tests \
	&& NVRM=/boot/home/build/src/mesa-nvk/src/nouveau/vulkan/nvkmd/nvrm \
	&& O=\$NVRM/open-gpu-kernel-modules \
	&& gcc -O2 -g -o $name $name.c \$NVRM/nvRmApi.c \
		-I\$NVRM \
		-I\$O/src/common/sdk/nvidia/inc \
		-I\$O/src/nvidia/arch/nvalloc/common/inc \
		-I\$O/src/nvidia/arch/nvalloc/unix/include \
		-I\$O/src/nvidia/inc/kernel \
		-I/boot/home/build/src/mesa-nvk/src/nouveau/headers/nvidia \
		$*"
