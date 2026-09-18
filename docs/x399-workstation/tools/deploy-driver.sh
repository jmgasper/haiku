#!/bin/bash
# Install the built nvidia_rm add-ons on the workstation and power cycle it.
set -euo pipefail
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=10 ws-haiku"
OUT=/mnt/HaikuWork/x399/build/nvidia_rm/out
tar -C $OUT -cf - add-ons | $SSH 'rm -rf /boot/home/x399-stage && mkdir -p /boot/home/x399-stage && cd /boot/home/x399-stage && tar -xf -'
$SSH 'np=/boot/system/non-packaged/add-ons; s=/boot/home/x399-stage/add-ons
	cp $s/kernel/drivers/bin/nvidia_rm $np/kernel/drivers/bin/nvidia_rm.new
	cp $s/kernel/drivers/bin/nvidia_rm_modeset $np/kernel/drivers/bin/nvidia_rm_modeset.new
	cp $s/accelerants/nvidia_rm.accelerant $np/accelerants/nvidia_rm.accelerant.new 2>/dev/null || true
	sync
	for f in $np/kernel/drivers/bin/*.new $np/accelerants/*.new; do mv -f "$f" "${f%.new}"; done
	sync'
/mnt/HaikuWork/x399/tools/power-cycle.sh
for i in $(seq 80); do $SSH 'uptime' 2>/dev/null && break; sleep 5; done
sleep 20
$SSH 'ls /dev/graphics; grep -a "RmInitAdapter succeeded" /var/log/syslog | tail -1'
