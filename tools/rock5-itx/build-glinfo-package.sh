#!/usr/bin/env bash
# Cross-build Haiku's GLInfo against the same arm64 OpenGL stack as GLTeapot.
set -euo pipefail
source "$(dirname "$0")/env.sh"

sdk="$HAIKU_WORK/artifacts/mesa-reconstruction/20260915T131119Z-96b4bc/sysroot"
glu="$HAIKU_WORK/artifacts/mali-system-opengl-application-build/20260918T125722Z/glu-install/boot/system"
cross="$HAIKU_WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-"
host_tools="$HAIKU_WORK/build/arm64/objects/linux/x86_64/release/tools"
app="$HAIKU_SOURCE/src/tests/kits/opengl/glinfo"
build="$HAIKU_WORK/build/rock5-glinfo"
package="$HAIKU_WORK/rock5-image-extras/packages/rock5_glinfo-1.0.0-1-arm64.hpkg"

for needed in "$sdk/boot/system/develop/headers/os/opengl/GL/gl.h" \
        "$glu/develop/headers/os/opengl/GL/glu.h" \
        "$glu/lib/libGLU.so" "$host_tools/package/package"; do
    test -e "$needed" || { echo "Missing pinned GLInfo input: $needed" >&2; exit 1; }
done

mkdir -p "$build"
cd "$build"
"${cross}g++" --sysroot="$sdk" -std=gnu++17 -O2 -fPIC \
    -I"$HAIKU_SOURCE/headers/private/interface" \
    -I"$HAIKU_SOURCE/headers/libs/glut" \
    -I"$sdk/boot/system/develop/headers/os/opengl" \
    -I"$glu/develop/headers/os/opengl" \
    -c "$app"/*.cpp
"${cross}g++" --sysroot="$sdk" -std=gnu++17 -O2 -o GLInfo ./*.o \
    "$HAIKU_WORK/build/arm64/objects/haiku/arm64/release/kits/interface/libcolumnlistview.a" \
    -L"$glu/lib" \
    -L"$HAIKU_WORK/build/arm64/objects/haiku/arm64/release/kits/locale" \
    -Wl,-rpath-link,"$sdk/boot/system/lib" \
    -lbe -ltranslation -llocalestub -lsupc++ -lGLU -lGL
"$host_tools/rc/rc" -o GLInfo.rsrc "$app/GLInfo.rdef"

stage=$(mktemp -d "$HAIKU_WORK/tmp/glinfo-stage-XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
mkdir -p "$stage/apps" "$stage/data/deskbar/menu/Applications"
cp GLInfo "$stage/apps/GLInfo"
"${cross}strip" --strip-debug "$stage/apps/GLInfo"
"$host_tools/xres" -o "$stage/apps/GLInfo" GLInfo.rsrc
"$host_tools/resattr/resattr" -O -o "$stage/apps/GLInfo" GLInfo.rsrc
ln -s ../../../../apps/GLInfo "$stage/data/deskbar/menu/Applications/GLInfo"
cat > "$stage/.PackageInfo" <<'EOF'
name rock5_glinfo
version 1.0.0-1
architecture arm64
summary "GLInfo for the ROCK 5 ITX Haiku build"
description "The Haiku GLInfo application built for arm64 against the fork Mesa OpenGL stack."
packager "jmgasper"
vendor "jmgasper"
copyrights { "2009-2012 Haiku Inc." }
licenses { "MIT" }
provides {
    rock5_glinfo = 1.0.0
    app:GLInfo = 1.0.0
}
requires {
    haiku >= r1~beta6
}
EOF
rm -f "$package"
"$host_tools/package/package" create -C "$stage" "$package"
"$host_tools/package/package" list -p "$package"
"${cross}readelf" -d "$stage/apps/GLInfo" | grep NEEDED
sha256sum "$package"
