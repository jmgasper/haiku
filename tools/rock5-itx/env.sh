#!/usr/bin/env bash
# Source this file. All project writes belong to the dedicated mounted drive.
HAIKU_WORK=/mnt/HaikuWork
if ! mountpoint -q "$HAIKU_WORK"; then
    echo "Required filesystem is not mounted: $HAIKU_WORK" >&2
    return 1 2>/dev/null || exit 1
fi
HAIKU_SOURCE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
case "$HAIKU_SOURCE/" in
    "$HAIKU_WORK/"*) ;;
    *) echo "Clone this fork beneath $HAIKU_WORK/src first." >&2
       return 1 2>/dev/null || exit 1 ;;
esac
export HAIKU_WORK HAIKU_SOURCE
export TMPDIR="$HAIKU_WORK/tmp" TMP="$HAIKU_WORK/tmp" TEMP="$HAIKU_WORK/tmp"
export XDG_CACHE_HOME="$HAIKU_WORK/cache" CCACHE_DIR="$HAIKU_WORK/cache/ccache"
export PIP_CACHE_DIR="$HAIKU_WORK/cache/pip"
export PYTHONPYCACHEPREFIX="$HAIKU_WORK/cache/pycache"
export NANOKVM_SESSION="$HAIKU_WORK/state/nanokvm-session.json"
export PATH="$HAIKU_WORK/toolchains/bin:$HAIKU_WORK/toolchains/host/usr/bin:$PATH"
export HAIKU_JOBS="${HAIKU_JOBS:-8}"
if [[ ! "$HAIKU_JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "HAIKU_JOBS must be a positive integer" >&2
    return 1 2>/dev/null || exit 1
fi
mkdir -p "$TMPDIR" "$XDG_CACHE_HOME" "$CCACHE_DIR" "$PIP_CACHE_DIR" \
    "$HAIKU_WORK/state" "$HAIKU_WORK/artifacts" "$HAIKU_WORK/build" \
    "$HAIKU_WORK/toolchains/bin"
