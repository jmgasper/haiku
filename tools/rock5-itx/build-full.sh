#!/usr/bin/env bash
# Build and record the regular ROCK 5 ITX arm64 image and its local inputs.
set -euo pipefail
source "$(dirname "$0")/env.sh"
exec 9>"$HAIKU_WORK/state/build.lock"
flock -n 9 || { echo "Another build owns the build lock." >&2; exit 1; }

for needed in \
    "$HAIKU_WORK/rock5-image-extras/develop/fluidlite/lib/libfluidlite.a" \
    "$HAIKU_WORK/rock5-image-extras/packages/rock5_glinfo-1.0.0-1-arm64.hpkg" \
    "$HAIKU_WORK/rock5-image-extras/packages/amp-0.2.0~alpha-1-arm64.hpkg" \
    "$HAIKU_WORK/rock5-image-extras/packages/kiri-0.0.1~alpha-1-arm64.hpkg" \
    "$HAIKU_WORK/rock5-image-extras/packages/turbochook-0.1.2~alpha-1-arm64.hpkg"; do
    test -s "$needed" || { echo "Missing full-image input: $needed" >&2; exit 1; }
done

cd "$HAIKU_WORK/build/arm64"
log="$HAIKU_WORK/artifacts/rock5full-build-$(date -u +%Y%m%dT%H%M%SZ).log"
python3 "$HAIKU_SOURCE/tools/rock5-itx/build_info.py" start @rock5full-mmc
if ! jam -q -j"$HAIKU_JOBS" @rock5full-mmc > "$log" 2>&1; then
    tail -60 "$log" >&2
    exit 1
fi
python3 "$HAIKU_SOURCE/tools/rock5-itx/build_info.py" finish
printf 'Build: %s\nRecord: %s\n' "$log" "$HAIKU_WORK/build/arm64/full-build-record.json"
