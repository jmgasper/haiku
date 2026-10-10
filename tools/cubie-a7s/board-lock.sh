#!/usr/bin/env bash
# Run a command while holding the Cubie A7S lab board, so that only one
# deploy-and-test runs at a time. Wrap the whole iteration (lab-run.sh and
# the checks after it) when several sessions share the board:
#
#   board-lock.sh bash -c 'lab-run.sh image; haiku-sh.sh "..."'
#
# lab-run.sh takes the lock itself when it is run on its own. Nested calls
# (CUBIE_BOARD_LOCKED set) run the command directly. Waits up to
# CUBIE_BOARD_WAIT seconds (default 3600).
set -euo pipefail
if [[ -n ${CUBIE_BOARD_LOCKED:-} ]]; then
	exec "$@"
fi
lock=${CUBIE_STATE:-/mnt/HaikuWork/cubie/state}/board.lock
exec 9>> "$lock"
if ! flock -n 9; then
	echo "board-lock: waiting for the board ($(cat "$lock.owner" 2>/dev/null))" >&2
	flock -w "${CUBIE_BOARD_WAIT:-3600}" 9 \
		|| { echo "board-lock: timed out" >&2; exit 75; }
fi
echo "$$ ${CUBIE_BOARD_OWNER:-$(whoami)} since $(date -u +%H:%MZ)" > "$lock.owner"
export CUBIE_BOARD_LOCKED=1
"$@"
