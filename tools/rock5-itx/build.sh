#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/env.sh"
exec 9>"$HAIKU_WORK/state/build.lock"
flock -n 9 || { echo "Another build owns the build lock." >&2; exit 1; }
buildtools="$HAIKU_WORK/src/buildtools"
output="$HAIKU_WORK/build/arm64"
lock="$HAIKU_SOURCE/tools/rock5-itx/sources.json"
revision=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["buildtools_revision"])' "$lock")
if [[ ! -d "$buildtools/.git" ]]; then
    git clone --filter=blob:none https://github.com/haiku/buildtools.git "$buildtools"
    git -C "$buildtools" checkout --detach "$revision"
fi
if [[ $(git -C "$buildtools" rev-parse HEAD) != "$revision" ]]; then
    echo "Buildtools revision differs from sources.json; inspect before rebuilding." >&2
    exit 1
fi
for command in gcc g++ make bison flex makeinfo autoheader automake awk nasm wget unzip xorriso mcopy python3; do
    command -v "$command" >/dev/null || { echo "Missing host tool: $command (see docs/rock5-itx/README.md)" >&2; exit 1; }
done
if ! git -C "$HAIKU_SOURCE" describe --tags --match='hrev*' HEAD >/dev/null 2>&1; then
    # The GitHub mirror omits the hrev tags used by Haiku's version generator.
    tag=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["haiku_revision_tag"])' "$lock")
    git -C "$HAIKU_SOURCE" fetch --quiet https://review.haiku-os.org/haiku.git \
        "refs/tags/$tag:refs/tags/$tag"
fi
if [[ ! -x "$HAIKU_WORK/toolchains/bin/jam" ]]; then
    (cd "$buildtools/jam" && make && ./jam0 -sBINDIR="$HAIKU_WORK/toolchains/bin" install)
fi
mkdir -p "$output"
cd "$output"
if [[ ! -f build/BuildConfig ]]; then
    "$HAIKU_SOURCE/configure" -j"$HAIKU_JOBS" \
        --cross-tools-source "$buildtools" --build-cross-tools arm64
fi
if [[ ! -f UserBuildConfig ]]; then
    ln -s "$HAIKU_SOURCE/tools/rock5-itx/UserBuildConfig" UserBuildConfig
fi
target=@minimum-mmc
if [[ $# -gt 0 ]]; then
    echo "Usage: $0 (builds the pinned @minimum-mmc profile)" >&2; exit 1
fi
log="$HAIKU_WORK/artifacts/build-$(date -u +%Y%m%dT%H%M%SZ).log"
echo "Building $target with $HAIKU_JOBS jobs. Log: $log"
python3 "$HAIKU_SOURCE/tools/rock5-itx/build_info.py" start "$target"
if ! jam -q -j"$HAIKU_JOBS" "$target" >"$log" 2>&1; then
    tail -60 "$log" >&2
    exit 1
fi
python3 "$HAIKU_SOURCE/tools/rock5-itx/build_info.py" finish
echo "Build succeeded. Log: $log"
