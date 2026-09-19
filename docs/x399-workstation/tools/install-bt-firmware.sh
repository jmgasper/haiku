#!/bin/bash
# Put the MediaTek Bluetooth firmware where the driver looks for it.
#
# A MT7922 answers no Bluetooth command at all until it has been given this,
# so the radio is useless without it. It is MediaTek's, redistributed by the
# linux-firmware project, and it is not ours to keep in this tree - so it is
# fetched rather than committed.
#
# The driver works out the file name from the radio itself, and looks in
# data/firmware/h2generic under the non-packaged and then the system data
# directories.
set -euo pipefail
X399=/mnt/HaikuWork/x399
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
REMOTE=/boot/system/non-packaged/data/firmware/h2generic
BASE=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek

# The radio names the one it wants; these cover the parts this driver reaches.
FIRMWARE="${1:-BT_RAM_CODE_MT7922_1_1_hdr.bin}"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "fetching $FIRMWARE"
curl -fsS -o "$WORK/$FIRMWARE" "$BASE/$FIRMWARE"

# A firmware image opens with a date stamp and the platform it was built for,
# so a redirect or an error page is easy to catch before it is installed.
if ! head -c 20 "$WORK/$FIRMWARE" | grep -q "ALPS"; then
	echo "what came back does not look like a firmware image" >&2
	exit 1
fi
echo "got $(stat -c %s "$WORK/$FIRMWARE") bytes"

$SSH "mkdir -p $REMOTE"
scp -F $X399/ssh/config "$WORK/$FIRMWARE" ws-haiku:$REMOTE/ >/dev/null
$SSH "sync; ls -l $REMOTE/$FIRMWARE"
echo "installed - the driver will use it the next time the radio is opened"
