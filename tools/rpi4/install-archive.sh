#!/usr/bin/env bash
# Build the boot archive, put it on the board's FAT volume and restart.
set -euo pipefail
H=${1:-192.168.1.214}
cd /mnt/HaikuWork/rpi4/build
source ../haiku/tools/rock5-itx/env.sh
jam -q -j12 @rpi4-airos build airos-rpi-boot-archive > build-archive.log 2>&1 || { grep -n "error" build-archive.log | cut -c1-300; exit 1; }
T=../haiku/tools/rpi4
python3 $T/send.py $H airos-boot.tgz /boot/home/airos-boot.tgz.staged
python3 $T/shell.py $H 'mkdir -p /tmp/fat; mount -t fat /dev/disk/mmc/0/0 /tmp/fat; cp /boot/home/airos-boot.tgz.staged /tmp/fat/airos-boot.tgz && sync && unmount /tmp/fat; sync; echo archive-installed' 60
timeout 30 python3 $T/shell.py $H 'shutdown -r' 15 >/dev/null 2>&1 || true
sleep 25
until timeout 8 python3 $T/shell.py $H 'echo up' 5 2>/dev/null | grep -q '^up'; do sleep 5; done
echo "board up"
