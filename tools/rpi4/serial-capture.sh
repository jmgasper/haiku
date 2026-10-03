#!/usr/bin/env bash
# Capture the Raspberry Pi's serial console (GPIO 14/15, 115200 8N1) through
# the NanoKVM's UART for a number of seconds.
#   serial-capture.sh <seconds> <log file> [tty, default /dev/ttyS1]
# Only one reader at a time: two readers split the bytes.
set -euo pipefail
SECONDS_TO_RUN=$1
LOG=$2
TTY=${3:-${RPI4_KVM_TTY:-/dev/ttyS1}}
KVM=(ssh -F "${NANOKVM_SSH_CONFIG:-/mnt/HaikuWork/nanokvm/.ssh/config}" nanokvm)
mkdir -p "$(dirname "$LOG")"
"${KVM[@]}" "pkill -f 'cat $TTY' 2>/dev/null; stty -F $TTY 115200 raw -echo -crtscts" || true
timeout "$SECONDS_TO_RUN" "${KVM[@]}" "cat $TTY" > "$LOG" 2>/dev/null || true
"${KVM[@]}" "pkill -f 'cat $TTY' 2>/dev/null" || true
