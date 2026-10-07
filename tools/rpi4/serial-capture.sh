#!/usr/bin/env bash
# Capture the Raspberry Pi's serial console (GPIO 14/15, 115200 8N1) through
# the NanoKVM's UART for a number of seconds.
#   serial-capture.sh <seconds> <log file> [tty, default /dev/ttyS1]
# Only one reader at a time: two readers split the bytes.
set -euo pipefail
SECONDS_TO_RUN=$1
LOG=$2
TTY=${3:-${RPI4_KVM_TTY:-/dev/ttyS1}}
[[ $SECONDS_TO_RUN =~ ^[1-9][0-9]*$ && $TTY =~ ^/dev/ttyS[0-9]+$ ]] || exit 2
KVM=(ssh -F "${NANOKVM_SSH_CONFIG:-/mnt/HaikuWork/nanokvm/.ssh/config}" nanokvm)
mkdir -p "$(dirname "$LOG")"
exec 9>"${RPI4_STATE:-/mnt/HaikuWork/rpi4/state}/serial.lock"
flock -n 9 || { echo 'Another serial capture is running' >&2; exit 1; }
# A pkill pattern for "cat $TTY" also matches the SSH command's own shell,
# killing it before stty runs. Bound the reader on the KVM itself instead,
# so an interrupted local SSH session cannot leave a competing reader.
status=0
timeout "$((SECONDS_TO_RUN + 5))" "${KVM[@]}" \
	"stty -F $TTY 115200 raw -echo -crtscts clocal || exit; \
	cat $TTY & reader=\$!; \
	(sleep $SECONDS_TO_RUN; kill \$reader 2>/dev/null) & timer=\$!; \
	trap 'kill \$reader \$timer 2>/dev/null' EXIT; \
	trap 'exit 143' HUP INT TERM; wait \$reader" \
	> "$LOG" || status=$?
case $status in
	0|124|143) ;;
	*) echo "Serial capture failed (status $status)" >&2; exit "$status";;
esac
