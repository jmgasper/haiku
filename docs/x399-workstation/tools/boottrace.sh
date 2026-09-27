#!/bin/sh
# Boot timeline for the X399: every second for 15 minutes, which servers are
# running, whether the wired network has an address, and the last Bluetooth
# LE log line, written with seconds since the kernel started whenever any of
# it changes. The syslog cannot give this: it has no times, and the kernel's
# lines reach it in batches, out of order with everything else.
d=/boot/home/x399-tests
out=$d/boottrace-$(date +%m%d-%H%M%S).txt
boot=$(python3 -c 'import time; print(int(time.time() - time.monotonic()))')
end=$(( boot + 900 ))
prev=""
echo "boot at $(date -d @$boot +%T 2>/dev/null || echo $boot)" > $out
while [ $(date +%s) -lt $end ]; do
	servers=$(ps | awk '{print $1}' | grep -o -E "(bluetooth_server|input_server|net_server|app_server|Tracker|Deskbar|launch_daemon|registrar)$" | sort -u | tr '\n' ' ')
	net=$(ifconfig /dev/net/ipro1000/0 2>/dev/null | grep -o "inet addr: [0-9.]*")
	h2=$(listimage 1 2>/dev/null | grep -c h2generic)
	le=$(tail -n 1 /boot/home/config/var/log/bluetooth_le.log 2>/dev/null | cut -c25-140)
	state="$servers | $net | h2generic $h2 | $le"
	if [ "$state" != "$prev" ]; then
		echo "+$(( $(date +%s) - boot ))s $state" >> $out
		prev=$state
	fi
	sleep 1
done
