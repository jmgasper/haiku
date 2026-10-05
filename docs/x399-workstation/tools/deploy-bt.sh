#!/bin/bash
# Build and install the h2generic Bluetooth transport driver, then power cycle
# and put the radio back on its feet.
#
# The driver goes in the user's non-packaged directory because that is the
# first place the kernel looks, and the one that ships with Haiku is already in
# the last. A legacy driver is found through its dev/ tree rather than by its
# binary, so the symlink matters as much as the file does.
#
# The radio loses its firmware when the power goes, and it answers nothing at
# all without it, so reloading is part of coming back up rather than something
# to remember separately.
set -euo pipefail
X399=/mnt/HaikuWork/x399
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
BUILT=$X399/build/x86_64/objects/haiku/x86_64/release/add-ons/kernel/drivers/bluetooth/h2/h2generic/h2generic
REMOTE=/boot/home/config/non-packaged/add-ons/kernel/drivers

cd $X399/build/x86_64
source $X399/tools/env.sh
jam -q h2generic 2>&1 | grep -E "error|Error|warning:" || true
[ -f "$BUILT" ] || { echo "the driver did not build" >&2; exit 1; }

$SSH "mkdir -p $REMOTE/bin $REMOTE/dev/bluetooth/h2"
scp -F $X399/ssh/config "$BUILT" ws-haiku:$REMOTE/bin/h2generic >/dev/null
$SSH "ln -sf ../../../bin/h2generic $REMOTE/dev/bluetooth/h2/h2generic && sync"
echo "installed the h2generic driver"

$X399/tools/power-cycle.sh
for i in $(seq 80); do $SSH 'uptime' >/dev/null 2>&1 && break; sleep 5; done
sleep 10

$SSH 'cd /boot/home/tests
	kill -9 $(ps | grep "[b]luetooth_server" | awk "{print \$2}") 2>/dev/null
	./btfw /dev/bus/usb/0/20 2>&1 | tail -2'

echo "--- what the driver found:"
$SSH 'grep -a "h2generic" /var/log/syslog | tail -6'
