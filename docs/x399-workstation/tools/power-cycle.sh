#!/bin/bash
# Full power cycle of the X399 workstation: clean shutdown when Haiku is
# reachable, forced power-off otherwise, then power on. Warm resets leave the
# GTX 1080 Ti in a bad state, so they are never used.
#
# The power LED is read from the NanoKVM GPIO. In S3 it blinks, so "off" is
# only trusted after it stayed off for several consecutive samples.
set -uo pipefail
K=/mnt/HaikuWork/x399/tools/kvm
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=5 ws-haiku"
led() { $K status 2>/dev/null | python3 -c 'import sys,json; print(int(json.load(sys.stdin)["/api/vm/gpio"]["pwr"]))' 2>/dev/null || echo 1; }
# returns success when the LED stayed off for $1 consecutive seconds
stays_off() {
	local n=0
	for i in $(seq $(( $1 * 3 ))); do
		if [ "$(led)" = 0 ]; then n=$((n + 1)); [ $n -ge $1 ] && return 0; else n=0; fi
		sleep 1
	done
	return 1
}

$SSH 'sync; shutdown' >/dev/null 2>&1 && sleep 20
if ! stays_off 12; then
	$K power power --duration 6000 >/dev/null || true
	stays_off 12 || { echo "workstation did not power off" >&2; exit 1; }
fi
sleep 5
$K power power --duration 800 >/dev/null
echo "powered on"
