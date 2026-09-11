#!/usr/bin/env bash
# Build, package, verify QEMU userspace startup, then observe and recover hardware.
set -euo pipefail
source "$(dirname "$0")/env.sh"
exec 8>"$HAIKU_WORK/state/iteration.lock"
flock -n 8 || { echo "Another iteration is active." >&2; exit 1; }
bash "$HAIKU_SOURCE/tools/rock5-itx/check.sh"
bash "$HAIKU_SOURCE/tools/rock5-itx/build.sh"
python3 "$HAIKU_SOURCE/tools/rock5-itx/lab.py" artifact \
    "$HAIKU_WORK/build/arm64/haiku-arm64-mmc.image" > "$HAIKU_WORK/state/latest-artifact.json"
manifest=$(python3 -c 'import json,sys,pathlib; print(pathlib.Path(json.load(open(sys.argv[1]))["image"]).with_suffix(".json"))' \
    "$HAIKU_WORK/state/latest-artifact.json")
python3 "$HAIKU_SOURCE/tools/rock5-itx/lab.py" qemu "$manifest" --el2 --seconds 60 \
    --expect 'Running first login script .*default_deskbar_items.sh'
python3 "$HAIKU_SOURCE/tools/rock5-itx/lab.py" cycle "$manifest" --seconds 60
