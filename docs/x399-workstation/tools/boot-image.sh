#!/bin/bash
# Power-cycle the X399 workstation and boot a NanoKVM image via the UEFI boot menu.
# usage: boot-image.sh <remote image name|none> [--menu]
# The BIOS boot priority lists the NanoKVM UEFI CD first, so an attached image
# boots without the F8 menu; with no image attached the NVMe boots.
set -uo pipefail
export NANOKVM_PASSWORD=${NANOKVM_PASSWORD:-}
/mnt/HaikuWork/x399/tools/kvm status >/dev/null 2>&1 || \
	NANOKVM_URL=http://192.168.1.22 NANOKVM_SESSION=/mnt/HaikuWork/x399/state/nanokvm-session.json \
	NANOKVM_PASSWORD=$(cat /mnt/HaikuWork/x399/state/nanokvm-password) \
	/mnt/HaikuWork/nanokvm/.venv/bin/python /mnt/HaikuWork/x399/tools/kvm.py login >/dev/null
K=/mnt/HaikuWork/x399/tools/kvm
name=$1
have_video() { $K screenshot /mnt/HaikuWork/tmp/x399-probe.jpg >/dev/null 2>&1; }
ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=5 ws-haiku 'sync; shutdown' >/dev/null 2>&1 || true
for i in $(seq 45); do have_video || break; sleep 2; done
if have_video; then
	$K power power --duration 6000 >/dev/null || true
	for i in $(seq 20); do have_video || break; sleep 2; done
fi
sleep 10
if [ "$name" = none ]; then
	$K unmount >/dev/null
else
	$K mount /data/$name --cdrom >/dev/null
fi
sleep 3
$K power power --duration 800 >/dev/null
if [ "${2:-}" = "--menu" ]; then
	for i in $(seq 30); do $K key F8 >/dev/null 2>&1; sleep 1; done
	$K screenshot /mnt/HaikuWork/x399/evidence/bootmenu-last.jpg >/dev/null 2>&1
	$K key ENTER >/dev/null
fi
echo "booting $name"
