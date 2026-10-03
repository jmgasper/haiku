#!/usr/bin/env bash
# Replace a kernel driver on the running board and restart it.
#   install-driver.sh <local file> <path on the board> [host]
# The file is staged outside the drivers directory: devfs loads anything
# that appears there (a "name.new" next to a running driver becomes a second
# instance on the same hardware) and reloads a driver whose file changes.
set -euo pipefail
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
FILE=$1
TARGET=$2
HOST=${3:-192.168.1.209}
"$TOOLS/send.py" "$HOST" "$FILE" /boot/home/driver.staged >/dev/null
timeout 30 "$TOOLS/shell.py" "$HOST" \
    "mv /boot/home/driver.staged '$TARGET'; sync; shutdown -r" 10 >/dev/null 2>&1 || true
sleep 20
for i in $(seq 1 40); do
    if timeout 8 "$TOOLS/shell.py" "$HOST" 'echo up' 5 2>/dev/null | grep -q '^up'; then
        echo "restarted with $(basename "$TARGET")"
        exit 0
    fi
    sleep 5
done
echo "the board did not come back" >&2
exit 1
