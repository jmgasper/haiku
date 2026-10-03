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

name_of() { # package file -> package name (everything before the version)
    basename "$1" | sed 's/-[0-9][^-]*-[0-9]*-[a-z0-9_]*\.hpkg$//'
}

mkdir -p "$OUT"
rm -f "$OUT"/*.hpkg
declare -A own=()
for package in "$OWN"/*.hpkg; do
    [ -e "$package" ] || continue
    own[$(name_of "$package")]=1
    cp -p "$package" "$OUT/"
done
for package in "$BASE"/*.hpkg; do
    [ -e "$package" ] || continue
    if [ -n "${own[$(name_of "$package")]:-}" ]; then
        echo "replaced: $(basename "$package")"
        continue
    fi
    cp -p "$package" "$OUT/"
done
ls "$OUT"
