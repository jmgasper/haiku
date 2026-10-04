#!/usr/bin/env bash
# How many pictures airTime shows per second of film on the lab board, over a
# stretch of steady playing.
#   airtime-rate.sh "<environment>" <file on the board> [seconds, default 20]
#   airtime-rate.sh "AIRTIME_NO_HARDWARE=1" multi.mkv
# AIRTIME=<path> picks the binary (default /boot/system/apps/airTime),
# RPI4_HOST the board.
set -euo pipefail
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HOST=${RPI4_HOST:-192.168.1.209}
AT=${AIRTIME:-/boot/system/apps/airTime}
HEY="hey application/x-vnd.airOS-airTime"
ENVIRONMENT=$1
FILE=$2
SECONDS_TO_PLAY=${3:-20}

# ps prints the team's id fourth from the end of the line
"$TOOLS/shell.py" "$HOST" "for t in \$(ps | grep '[a]irTime' | awk '{print \$(NF-3)}'); do kill -9 \$t; done 2>/dev/null; sleep 1; cd /boot/home; $ENVIRONMENT $AT $FILE > /boot/home/airtime.log 2>&1 &
sleep 6; $HEY get Stats of Window 0 | grep --color=never result; sleep $SECONDS_TO_PLAY; $HEY get Stats of Window 0 | grep --color=never result; $HEY get Decoder of Window 0 | grep --color=never result; $HEY quit > /dev/null" 120 2>&1 | python3 -c '
import re, sys
lines = [l for l in sys.stdin if "position=" in l or "audio:" in l]
def value(line, key):
    return float(re.search(key + r"=(-?[\d.]+)", line).group(1))
first, last = lines[0], lines[1]
film = value(last, "position") - value(first, "position")
shown = value(last, "shown") - value(first, "shown")
dropped = value(last, "dropped") - value(first, "dropped")
print("%.1f shown/s, %d left out in %.1f s (before that: %d); decode %.1f, "
    "compose %.1f, draw %.1f ms; %s" % (shown / film, dropped, film,
    value(first, "dropped"), value(last, "decode"), value(last, "compose"),
    value(last, "draw"), lines[2].split(": ", 1)[1].strip()))'
