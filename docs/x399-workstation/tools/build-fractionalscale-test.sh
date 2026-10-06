#!/bin/bash
# Cross-build the device-pixel border and vector icon regression test.
# Run it in a visible desktop; see ../FRACTIONAL-SCALING.md.
set -euo pipefail
X399=${X399:-/mnt/HaikuWork/x399}
BUILD=${BUILD:-/mnt/HaikuWork/build/master-x86_64}
HAIKU=$(cd "$(dirname "$0")/../../.." && pwd)
TESTS=$HAIKU/docs/x399-workstation/tests
OUT=${OUT:-/mnt/HaikuWork/artifacts/fractional-scaling/build}

TOOLS=${TOOLS:-$X399/build/x86_64/cross-tools-x86_64/bin/x86_64-unknown-haiku}
OBJ=$BUILD/objects/haiku/x86_64/release
GCC_SYSLIBS=$(ls -d "$BUILD"/build_packages/gcc_syslibs_devel-*)
GCC_SYSLIBS_RUNTIME=$(ls -d "$BUILD"/build_packages/gcc_syslibs-*)
GCCLIB=$(dirname "$($TOOLS-gcc -print-libgcc-file-name)")

INCLUDES=(
	-I"$GCC_SYSLIBS/develop/headers/c++"
	-I"$GCC_SYSLIBS/develop/headers/c++/x86_64-unknown-haiku"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include-fixed"
	-I"$HAIKU/headers/glibc" -I"$HAIKU/headers/posix" -I"$HAIKU/headers"
	-I"$HAIKU/headers/os" -I"$HAIKU/headers/os/game" -I"$HAIKU/headers/os/add-ons/graphics"
	-I"$HAIKU/headers/os/app" -I"$HAIKU/headers/os/interface"
	-I"$HAIKU/headers/os/kernel" -I"$HAIKU/headers/os/storage"
	-I"$HAIKU/headers/os/support" -I"$HAIKU/headers/private/graphics/common")

mkdir -p "$OUT"
for f in "$TESTS/fractionalscale.cpp"; do
	$TOOLS-g++ -O1 -g -nostdinc -Wall "${INCLUDES[@]}" -c "$f" \
		-o "$OUT/$(basename "$f" .cpp).o"
done
$TOOLS-g++ -nostdlib -o "$OUT/fractionalscale" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbegin.o" \
	"$OBJ/system/glue/start_dyn.o" "$OBJ/system/glue/init_term_dyn.o" \
	"$OUT/fractionalscale.o" "$OBJ/kits/game/libgame.so" "$OBJ/kits/libbe.so" "$OBJ/system/libroot/libroot.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libstdc++.so" "$GCC_SYSLIBS_RUNTIME/lib/libgcc_s.so" \
	"$GCCLIB/libgcc.a" "$GCCLIB/crtend.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined -Wl,-rpath-link,"$BUILD"
echo "built $OUT/fractionalscale"

