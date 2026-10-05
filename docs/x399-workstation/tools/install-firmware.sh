#!/bin/bash
# Put the MediaTek firmware where the drivers look for it.
#
# Neither half of a MT7922 does anything until it has been given firmware: the
# radio answers no Bluetooth command at all, and the Wi-Fi side stays a
# bootloader. It is MediaTek's, redistributed by the linux-firmware project,
# and it is not ours to keep in this tree - so it is fetched rather than
# committed.
#
# Each driver works out which file it wants from the part itself, and looks
# under data/firmware in the non-packaged and then the system data directory.
set -euo pipefail
X399=/mnt/HaikuWork/x399
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
BT_REMOTE=/boot/system/non-packaged/data/firmware/h2generic
WIFI_REMOTE=/boot/system/non-packaged/data/firmware/mt7922
BASE=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fetch() { # fetch <file> <where it goes>
	echo "fetching $1"
	if ! curl -fsS -o "$WORK/$1" "$BASE/$1"; then
		echo "  could not be fetched" >&2
		return 1
	fi
	# Anything this small is an error page rather than a firmware image.
	local size
	size=$(stat -c %s "$WORK/$1")
	if [ "$size" -lt 10000 ]; then
		echo "  what came back is $size bytes, which is not a firmware image" >&2
		return 1
	fi
	$SSH "mkdir -p $2"
	scp -F $X399/ssh/config "$WORK/$1" ws-haiku:$2/ >/dev/null
	echo "  $size bytes installed in $2"
}

fetch BT_RAM_CODE_MT7922_1_1_hdr.bin "$BT_REMOTE"

# The Wi-Fi side wants two: the patch its bootloader takes first, and the
# code it runs afterwards.
fetch WIFI_MT7922_patch_mcu_1_1_hdr.bin "$WIFI_REMOTE"
fetch WIFI_RAM_CODE_MT7922_1.bin "$WIFI_REMOTE"

$SSH "sync"
echo "done - each driver picks its own up the next time the part is opened"
