#!/bin/bash
# One iteration of mt7922wifi testing: power-cycle the workstation (a staged
# drivers/bin/mt7922wifi.new swaps in at boot, x399-driver-safety.sh), restart
# the synced log flusher, wait out the driver's two-minute boot gate, bring
# the card in by touching the driver, and print what the driver said.
#
# Never replace the binary of a loaded mt7922wifi instead: devfs reloads it
# underneath itself and that has stopped the machine.
#
# usage: wifi-test-cycle.sh [seconds to wait after attaching (default 40)]
set -uo pipefail
X399=/mnt/HaikuWork/x399
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=5 ws-haiku"
BIN=/boot/home/config/non-packaged/add-ons/kernel/drivers/bin/mt7922wifi
settle=${1:-40}

source $X399/tools/env.sh
export NANOKVM_PASSWORD=${NANOKVM_PASSWORD:-$(cat $X399/state/nanokvm-password)}
/mnt/HaikuWork/nanokvm/.venv/bin/python $X399/tools/kvm.py login >/dev/null 2>&1

$X399/tools/power-cycle.sh || exit 1
for i in $(seq 60); do
	$SSH true 2>/dev/null && break
	sleep 8
done
$SSH "sha256sum $BIN | cut -c1-16; nohup /boot/home/wifi-flush.sh >/dev/null 2>&1 &
	until uptime | grep -qE 'up +0:0[2-9]|up +[0-9]+:[1-5][0-9]'; do sleep 5; done
	touch $BIN; sleep $settle; ls /dev/net
	grep -a 'mtk)' /var/log/syslog | grep -v 'ring . slot' | tail -60" | cut -c1-200
