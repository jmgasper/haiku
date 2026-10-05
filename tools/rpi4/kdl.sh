#!/usr/bin/env bash
# Run kernel debugger commands over the board's serial console.
#   kdl.sh <seconds per command> <command>...
set -euo pipefail
WAIT=$1; shift
KVM=(ssh -F /mnt/HaikuWork/nanokvm/.ssh/config -o ConnectTimeout=10 nanokvm)
script='stty -F /dev/ttyS1 115200 raw -echo -crtscts; cat /dev/ttyS1 > /tmp/kdl.out & pid=$!; sleep 1;'
for c in "$@"; do
    script+=" printf '%s\n' '$c' > /dev/ttyS1; sleep $WAIT;"
done
script+=' kill $pid; cat /tmp/kdl.out'
"${KVM[@]}" "$script" | sed 's/\x1b\[[0-9;]*m//g'
