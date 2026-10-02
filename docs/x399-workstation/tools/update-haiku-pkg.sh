#!/bin/bash
# Replace the running workstation's haiku system package with the local build and reboot.
set -euo pipefail
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=10 ws-haiku"
PKG=/mnt/HaikuWork/x399/build/x86_64/objects/haiku/x86_64/packaging/packages/haiku.hpkg
name=$($SSH 'ls /boot/system/packages/ | grep "^haiku-r1"')
$SSH "mkdir -p /boot/home/x399-backup && cat > /boot/home/x399-backup/new-haiku.hpkg" < $PKG
remote=$($SSH "sha256sum /boot/home/x399-backup/new-haiku.hpkg" | cut -d' ' -f1)
[ "$remote" = "$(sha256sum $PKG | cut -d' ' -f1)" ] || { echo "hash mismatch" >&2; exit 1; }
$SSH "cp /boot/system/packages/$name /boot/home/x399-backup/previous-haiku.hpkg; mv -f /boot/home/x399-backup/new-haiku.hpkg /boot/system/packages/$name && sync"
/mnt/HaikuWork/x399/tools/power-cycle.sh
sleep 60
for i in $(seq 80); do
	$SSH 'uname -v' 2>/dev/null && exit 0
	sleep 5
done
echo "workstation did not come back" >&2
exit 1
