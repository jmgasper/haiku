#!/usr/bin/env bash
# Boot the Raspberry Pi loader in QEMU's raspi4b machine.
#   qemu-run.sh [seconds] [extra kernel arguments]
# Prints the serial log when the time is up. The boot archive is passed as
# the initrd if it was built.
#   RPI4_SD=<image>             attach an SD card image
#   RPI4_SCREENSHOT=<file.png>  save the frame buffer at the end
#   RPI4_QEMU_ARGS=...          more QEMU arguments
set -euo pipefail
WORK=/mnt/HaikuWork/rpi4
BUILD=${RPI4_BUILD:-$WORK/build}
QEMU=${QEMU:-/mnt/HaikuWork/build/qemu-10.2.0/qemu-system-aarch64}
SECONDS_TO_RUN=${1:-20}
shift || true
LOADER=$BUILD/objects/haiku/arm64/release/system/boot/rpi/haiku_loader.rpi
ARCHIVE=${RPI4_ARCHIVE:-$BUILD/airos-boot.tgz}
LOG=$(mktemp "${TMPDIR:-/mnt/HaikuWork/tmp}/rpi4-qemu.XXXXXX")
trap 'rm -f "$LOG" "$LOG.ppm"' EXIT
args=(-M raspi4b -kernel "$LOADER" -dtb "$WORK/firmware/qemu-rpi4.dtb"
    -append "airos.debug $*" -serial "file:$LOG" -display none -monitor stdio)
[ -f "$ARCHIVE" ] && args+=(-initrd "$ARCHIVE")
[ -n "${RPI4_SD:-}" ] && args+=(-drive "file=$RPI4_SD,if=sd,format=raw")
{
    sleep "$SECONDS_TO_RUN"
    [ -n "${RPI4_SCREENSHOT:-}" ] && echo "screendump $LOG.ppm" && sleep 1
    echo quit
} | "$QEMU" "${args[@]}" ${RPI4_QEMU_ARGS:-} >/dev/null 2>&1 || true
if [ -n "${RPI4_SCREENSHOT:-}" ] && [ -f "$LOG.ppm" ]; then
    python3 -c "from PIL import Image; import sys; Image.open(sys.argv[1]).save(sys.argv[2])" \
        "$LOG.ppm" "$RPI4_SCREENSHOT"
fi
cat "$LOG"
