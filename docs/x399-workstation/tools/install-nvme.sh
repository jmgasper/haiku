#!/bin/sh
# Run on a live X399 Haiku system: install the running system to the NVMe.
set -e
DEV=/dev/disk/nvme/0
[ -d /x399nvme ] && unmount /x399nvme 2>/dev/null || true
[ -d /x399efi ] && unmount /x399efi 2>/dev/null || true
if [ -n "$FORMAT" ]; then mkfs -t bfs -q $DEV/1 X399; fi
if [ -n "$FORMAT" ]; then mkfs -t fat -q $DEV/0 EFI; fi
mkdir -p /x399nvme /x399efi
mount -t bfs $DEV/1 /x399nvme
mount -t fat $DEV/0 /x399efi
mkdir -p /x399nvme/system /x399nvme/home/config
for d in packages settings cache var non-packaged; do
	[ -e /boot/system/$d ] && copyattr -d -r /boot/system/$d /x399nvme/system/
done
for d in packages settings cache var non-packaged; do
	[ -e /boot/home/config/$d ] && copyattr -d -r /boot/home/config/$d /x399nvme/home/config/
done
for f in /boot/home/*; do
	[ "$f" = /boot/home/config ] || copyattr -d -r "$f" /x399nvme/home/
done
mkdir -p /x399nvme/trash
mkdir -p /x399efi/EFI/BOOT
cp /boot/system/data/platform_loaders/haiku_loader.efi /x399efi/EFI/BOOT/BOOTX64.EFI
sync
ls -lR /x399efi
du -sh /x399nvme/system/packages
unmount /x399efi
unmount /x399nvme
echo INSTALL-DONE
