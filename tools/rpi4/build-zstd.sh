#!/usr/bin/env bash
# Supply the arm64 bootstrap repository's missing Zstandard build feature.
# Packages stay zlib-compressed so an older image can install the transition.
set -euo pipefail
umask 022
TOOLS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$TOOLS_DIR/../rock5-itx/env.sh"
ROOT=${RPI4_ZSTD_ROOT:-/mnt/HaikuWork/rpi4/zstd}
BUILD=${RPI4_BUILD:-/mnt/HaikuWork/rpi4/build}
CROSS=${RPI4_CROSS:-/mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-}
SYSROOT=${RPI4_SYSROOT:-/mnt/HaikuWork/build/summit-arm64/sysroot}
PACKAGE=$BUILD/objects/linux/x86_64/release/tools/package/package
VERSION=1.5.7
REVISION=1
SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
ARCHIVE=$ROOT/zstd-$VERSION.tar.gz
SOURCE=$ROOT/zstd-$VERSION
OUT=$ROOT/packages
mkdir -p "$ROOT" "$OUT"
if [[ ! -f $ARCHIVE ]]; then
    curl -fL --retry 3 -o "$ARCHIVE.part" \
        "https://github.com/facebook/zstd/releases/download/v$VERSION/zstd-$VERSION.tar.gz"
    printf '%s  %s\n' "$SHA256" "$ARCHIVE.part" | sha256sum -c -
    mv "$ARCHIVE.part" "$ARCHIVE"
fi
printf '%s  %s\n' "$SHA256" "$ARCHIVE" | sha256sum -c -
[[ -f $SOURCE/lib/zstd.h ]] || tar -xzf "$ARCHIVE" -C "$ROOT"

# Command-line LDFLAGS override make's per-target additions, so include both
# -shared and -pthread explicitly. The only dynamic dependency is libroot.
make -C "$SOURCE/lib" -j"$HAIKU_JOBS" libzstd \
    CC="${CROSS}gcc --sysroot=$SYSROOT" AR="${CROSS}ar" \
    UNAME_TARGET_SYSTEM=Haiku CFLAGS='-O3 -fPIC -fvisibility=hidden' \
    LDFLAGS='-shared -pthread -Wl,--hash-style=both' > "$ROOT/library-build.log" 2>&1

STAGE=$(mktemp -d "$ROOT/stage.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/base/lib" "$STAGE/base/data/licenses" \
    "$STAGE/devel/develop/headers" "$STAGE/devel/develop/lib" \
    "$STAGE/source/develop/sources/zstd-$VERSION-$REVISION/sources"
cp "$SOURCE/lib/libzstd.so.$VERSION" "$STAGE/base/lib/"
"${CROSS}strip" --strip-unneeded "$STAGE/base/lib/libzstd.so.$VERSION"
ln -s "libzstd.so.$VERSION" "$STAGE/base/lib/libzstd.so.1"
cp "$SOURCE/LICENSE" "$STAGE/base/data/licenses/Zstandard"
cp "$SOURCE/lib/zstd.h" "$SOURCE/lib/zstd_errors.h" \
    "$SOURCE/lib/zdict.h" "$STAGE/devel/develop/headers/"
ln -s "../../lib/libzstd.so.$VERSION" "$STAGE/devel/develop/lib/libzstd.so"
# Extract the pristine archive, not the directory containing build objects.
tar -xzf "$ARCHIVE" --strip-components=1 \
    -C "$STAGE/source/develop/sources/zstd-$VERSION-$REVISION/sources"

metadata() {
    local path=$1 name=$2 architecture=$3 provides=$4 requires=$5
    cat > "$path/.PackageInfo" <<EOF
name $name
version $VERSION-$REVISION
architecture $architecture
summary "Zstandard compression for air/OS arm64"
description "Pinned Zstandard $VERSION, built for air/OS arm64."
packager "air/OS"
vendor "air/OS"
copyrights "Meta Platforms, Inc. and affiliates"
licenses "BSD (3-clause)"
urls "https://github.com/facebook/zstd"
source-urls "https://github.com/facebook/zstd/releases/download/v$VERSION/zstd-$VERSION.tar.gz"
provides {
    $provides
}
requires {
    $requires
}
EOF
}
metadata "$STAGE/base" zstd arm64 \
    $'zstd = 1.5.7\n    lib:libzstd = 1.5.7 compat >= 1' 'haiku >= r1~beta6'
metadata "$STAGE/devel" zstd_devel arm64 \
    $'zstd_devel = 1.5.7\n    devel:libzstd = 1.5.7 compat >= 1' 'zstd == 1.5.7'
metadata "$STAGE/source" zstd_source source 'zstd_source = 1.5.7' ''

# Host tools associate attributes with inode numbers; fresh staging inodes
# must not inherit attributes belonging to earlier, deleted build outputs.
python3 - "$STAGE" "$BUILD/attributes" <<'PY'
import os
from pathlib import Path
import shutil
import sys
stage, attributes = map(Path, sys.argv[1:])
for path in [stage, *stage.rglob('*')]:
    directory = attributes / str(path.lstat().st_ino)
    if directory.is_dir():
        shutil.rmtree(directory)
PY
for kind in base devel source; do
    case "$kind" in
        base) name=zstd; architecture=arm64 ;;
        devel) name=zstd_devel; architecture=arm64 ;;
        source) name=zstd_source; architecture=source ;;
    esac
    file=$OUT/$name-$VERSION-$REVISION-$architecture.hpkg
    "$PACKAGE" create -q -z zlib -C "$STAGE/$kind" "$file"
    "$PACKAGE" list -a "$file" > "$file.manifest"
    sha256sum "$file"
done
# Jam does not track compiler flag changes. The package tool's format
# selector predates this feature in an existing bootstrap build; rebuild it
# with ZSTD_DEFAULT next time. A dependency on extracted feature headers
# would introduce a cycle because this host tool extracts those headers.
rm -f "$BUILD/objects/linux/x86_64/release/tools/package/package.o" \
    "$BUILD/objects/haiku/arm64/release/bin/package/package.o"
echo "Packages for HAIKU_ARM64_ZSTD_PACKAGES_DIR: $OUT"
