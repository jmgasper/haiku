#!/bin/bash
# Build the NVPlay player on the workstation and put it in ~/config/bin.
# The application resources have to be embedded or a Haiku program with a
# window hangs on its first AddChild without saying why.
set -eu
X399=${X399:-/mnt/HaikuWork/x399}
HAIKU=$X399/haiku
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"

$SSH 'mkdir -p /boot/home/build/nvplay'
scp -F $X399/ssh/config -q $HAIKU/src/apps/nvplay/NVPlay.cpp \
	$HAIKU/src/apps/nvplay/NVPlay.rdef ws-haiku:/boot/home/build/nvplay/
$SSH "cd /boot/home/build/nvplay \
	&& g++ -O2 -g -o NVPlay NVPlay.cpp -lbe -lmedia -ltracker \
	&& rc -o NVPlay.rsrc NVPlay.rdef \
	&& xres -o NVPlay NVPlay.rsrc \
	&& mimeset -f NVPlay \
	&& mkdir -p /boot/home/config/non-packaged/bin \
	&& cp NVPlay /boot/home/config/non-packaged/bin/NVPlay \
	&& echo installed /boot/home/config/non-packaged/bin/NVPlay"
