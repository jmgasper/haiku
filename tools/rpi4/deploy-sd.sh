#!/usr/bin/env bash
# Write an SD card image to the Raspberry Pi's card through the recovery OS,
# check it by reading it back, and boot the card.
#   deploy-sd.sh <image> [boot|noboot]
# The recovery OS (build-recovery-image.sh) is started if it is not running.
# With "boot" (the default) the recovery image is detached afterwards and the
# board power-cycled, so the EEPROM falls through to the card.
set -euo pipefail
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
STATE=${RPI4_STATE:-/mnt/HaikuWork/rpi4/state}
IMAGE=$1
MODE=${2:-boot}
SSH=(ssh -F "$STATE/ssh_config" rpi4-recovery)

if ! "${SSH[@]}" true 2>/dev/null; then
    echo "starting the recovery OS"
    "$TOOLS/power.py" off >/dev/null
    "$TOOLS/build-recovery-image.sh" attach >/dev/null
    # as below: switched on again at once, the board can stay dead
    sleep 8
    "$TOOLS/power.py" on >/dev/null
    for _ in $(seq 60); do
        sleep 3
        "${SSH[@]}" true 2>/dev/null && break
    done
    "${SSH[@]}" true
fi

size=$(stat -c %s "$IMAGE")
sum=$(sha256sum < "$IMAGE" | cut -d' ' -f1)
echo "writing $size bytes"
gzip -1 -c "$IMAGE" | "${SSH[@]}" \
    'gunzip -c | dd of=/dev/mmcblk0 bs=4M conv=fsync 2>/dev/null; sync; echo 3 > /proc/sys/vm/drop_caches'
written=$("${SSH[@]}" "head -c $size /dev/mmcblk0 | sha256sum | cut -d' ' -f1")
if [ "$written" != "$sum" ]; then
    echo "readback differs: $written, expected $sum" >&2
    exit 1
fi
echo "readback matches: $sum"

if [ "$MODE" = boot ]; then
    "${SSH[@]}" 'sync; poweroff' 2>/dev/null || true
    sleep 5
    "$TOOLS/power.py" off >/dev/null
    "$TOOLS/build-recovery-image.sh" detach >/dev/null
    # Switched on again at once, the board twice stayed dead (no serial
    # output, no boot) until it had been off for some seconds.
    sleep 8
    "$TOOLS/power.py" on >/dev/null
    echo "booting the card"
fi
