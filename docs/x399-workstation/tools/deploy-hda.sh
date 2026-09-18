#!/bin/bash
# Install the patched hda audio driver on the workstation and power cycle it.
#
# It goes into the user's non-packaged directory, not the system one. The
# kernel looks in B_USER_NONPACKAGED_ADDONS_DIRECTORY, B_USER_ADDONS_DIRECTORY,
# B_SYSTEM_NONPACKAGED_ADDONS_DIRECTORY and then B_SYSTEM_ADDONS_DIRECTORY, and
# a driver that ships with Haiku is already in the last of those. Putting a
# replacement in the system non-packaged directory is not enough - the packaged
# one still wins - so it has to go in the user's, which is searched first.
#
# A legacy driver is found through the dev/ tree rather than by its binary, so
# the symlink matters as much as the binary does.
set -euo pipefail
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=10 ws-haiku"
BUILT=/mnt/HaikuWork/x399/build/x86_64/objects/haiku/x86_64/release/add-ons/kernel/drivers/audio/hda/hda
REMOTE=/boot/home/config/non-packaged/add-ons/kernel/drivers

[ -f "$BUILT" ] || { echo "build it first: jam -q hda" >&2; exit 1; }

$SSH "mkdir -p $REMOTE/bin $REMOTE/dev/audio/hmulti"
scp -F /mnt/HaikuWork/x399/ssh/config "$BUILT" ws-haiku:$REMOTE/bin/hda >/dev/null
$SSH "ln -sf ../../../bin/hda $REMOTE/dev/audio/hmulti/hda && sync"
echo "installed the hda driver"

/mnt/HaikuWork/x399/tools/power-cycle.sh
for i in $(seq 80); do $SSH 'uptime' 2>/dev/null && break; sleep 5; done
sleep 15
$SSH 'grep -a "hda: converters" /var/log/syslog | tail -4; ls /dev/audio/hmulti/hda/'
