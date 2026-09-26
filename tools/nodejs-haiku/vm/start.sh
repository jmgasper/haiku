#!/bin/sh
# Start the Haiku build VM. Usage: start.sh [cdrom]
VM=$(dirname "$(readlink -f "$0")")
CD=""
BOOT="-boot c"
if [ "$1" = cdrom ]; then CD="-drive file=$VM/live.iso,media=cdrom,readonly=on"; BOOT="-boot d"; fi
exec qemu-system-x86_64 -name haiku-node -enable-kvm -cpu host -smp 12 -m 12G \
  -machine q35 \
  -device ahci,id=ahci \
  -drive file=$VM/disk.raw,format=raw,if=none,id=d0,cache=unsafe -device ide-hd,drive=d0,bus=ahci.0 \
  $CD $BOOT \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2223-:2222 -device e1000,netdev=n0 \
  -device qemu-xhci -device usb-tablet \
  -display none -vnc 127.0.0.1:47,password=on \
  -monitor unix:$VM/monitor.sock,server,nowait \
  -serial file:$VM/serial.log
