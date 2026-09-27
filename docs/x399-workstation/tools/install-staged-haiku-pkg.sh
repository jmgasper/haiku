#!/bin/bash
# Put the package staged by repack-haiku-pkg.sh in place of the installed
# haiku package, and optionally an nvidia_rm accelerant too, then power cycle.
#
# usage: install-staged-haiku-pkg.sh [<nvidia_rm.accelerant>]
#
# The installed package is kept as /boot/home/x399-backup/previous-haiku.hpkg.
# The accelerant goes in as nvidia_rm.accelerant.new; the boot job
# x399-driver-safety swaps it in before app_server loads it, so the one that
# is running is never overwritten underneath it.
set -euo pipefail
X399=${X399:-/mnt/HaikuWork/x399}
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
STAGE=/boot/home/x399-stage

if [ $# -gt 0 ]; then
	$SSH "cat > /boot/system/non-packaged/add-ons/accelerants/nvidia_rm.accelerant.new" < "$1"
	remote=$($SSH "sha256sum /boot/system/non-packaged/add-ons/accelerants/nvidia_rm.accelerant.new" | cut -d' ' -f1)
	[ "$remote" = "$(sha256sum "$1" | cut -d' ' -f1)" ] || { echo "accelerant upload damaged" >&2; exit 1; }
fi

$SSH "set -e
	name=\$(ls /boot/system/packages/ | grep '^haiku-r1')
	[ -f $STAGE/haiku.hpkg ]
	mkdir -p /boot/home/x399-backup
	cp /boot/system/packages/\$name /boot/home/x399-backup/previous-haiku.hpkg
	cp $STAGE/haiku.hpkg /boot/system/packages/\$name.new
	sync
	mv -f /boot/system/packages/\$name.new /boot/system/packages/\$name
	sync"

$X399/tools/power-cycle.sh
sleep 60
for i in $(seq 80); do
	$SSH 'uname -v' 2>/dev/null && exit 0
	sleep 5
done
echo "workstation did not come back" >&2
exit 1
