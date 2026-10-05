#!/bin/bash
# Enter S3 with a power-off checkpoint and record the power LED once per
# second. Usage: s3-probe.sh <checkpoint> [seconds] [press-after-seconds]
set -uo pipefail
K=/mnt/HaikuWork/x399/tools/kvm
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=10 ws-haiku"
cp=$1; secs=${2:-90}; press=${3:-0}
led() { $K status 2>/dev/null | python3 -c 'import sys,json; print(int(json.load(sys.stdin)["/api/vm/gpio"]["pwr"]), end="")'; }
$SSH "sync; nohup /boot/home/tests/x86suspend s3 ${FLAGS:-3} $cp > /dev/null 2>&1 &"
for i in $(seq $secs); do
	[ "$press" != 0 ] && [ "$i" = "$press" ] && { $K power power --duration 500 >/dev/null; printf '|'; }
	led; sleep 1
done
echo
