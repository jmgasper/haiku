#!/usr/bin/env bash
# Collect the application packages of the rpi4-airos image in one directory:
# the arm64 packages of the air/OS release (/mnt/HaikuWork/airos/packages-arm64),
# with any package that was rebuilt for this board
# (/mnt/HaikuWork/rpi4/packages-arm64) taking the place of its namesake.
# tools/rpi4/UserBuildConfig installs what is in the result.
#   stage-packages.sh [directory]
set -euo pipefail
BASE=${AIROS_PACKAGES:-/mnt/HaikuWork/airos/packages-arm64}
OWN=${RPI4_PACKAGES:-/mnt/HaikuWork/rpi4/packages-arm64}
OUT=${1:-/mnt/HaikuWork/rpi4/image-packages}
BUILD=${RPI4_BUILD:-/mnt/HaikuWork/rpi4/build}
PACKAGE=${RPI4_PACKAGE_TOOL:-$BUILD/objects/linux/x86_64/release/tools/package/package}
COMPRESSION=${RPI4_PACKAGE_COMPRESSION:-zstd}

name_of() { # package file -> package name (everything before the version)
    basename "$1" | sed 's/-[0-9][^-]*-[0-9]*-[a-z0-9_]*\.hpkg$//'
}

mkdir -p "$OUT"
# Recompress image copies only. Keep the source release packages available
# for older systems whose package readers do not yet support Zstandard.
STAGE=$(mktemp -d "$OUT/.stage.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
stage_package() {
    local input=$1 output=$STAGE/$(basename "$1")
    "$PACKAGE" recompress -q -z "$COMPRESSION" "$input" "$output"
}
declare -A own=()
for package in "$OWN"/*.hpkg; do
    [ -e "$package" ] || continue
    own[$(name_of "$package")]=1
    stage_package "$package"
done
for package in "$BASE"/*.hpkg; do
    [ -e "$package" ] || continue
    if [ -n "${own[$(name_of "$package")]:-}" ]; then
        echo "replaced: $(basename "$package")"
        continue
    fi
    stage_package "$package"
done
rm -f "$OUT"/*.hpkg
mv "$STAGE"/*.hpkg "$OUT/"
ls "$OUT"
