#!/usr/bin/env bash
# One lab iteration: write the card image through the recovery OS, boot the
# card, and capture the serial console.
#   cycle.sh <image> <seconds of serial capture> <log file>
set -euo pipefail
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
STATE=${RPI4_STATE:-/mnt/HaikuWork/rpi4/state}
IMAGE=$1
CAPTURE=$2
LOG=$3

"$TOOLS/deploy-sd.sh" "$IMAGE" noboot
ssh -F "$STATE/ssh_config" rpi4-recovery 'sync; poweroff' 2>/dev/null || true
sleep 6
"$TOOLS/power.py" off >/dev/null
"$TOOLS/build-recovery-image.sh" detach >/dev/null 2>&1
"$TOOLS/serial-capture.sh" "$CAPTURE" "$LOG" &
sleep 3
"$TOOLS/power.py" on >/dev/null
wait
echo "serial log: $LOG ($(stat -c %s "$LOG") bytes)"
