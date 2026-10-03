#!/usr/bin/env bash
# Cross-build what Summit's GL configuration (Skia, GL compositing, WebGL)
# needs on top of the arm64 dependencies of /mnt/HaikuWork/build/summit-arm64:
# FreeType, Expat, Fontconfig, HarfBuzz (with ICU) and libepoxy, into a prefix
# of its own. Sources are fetched to $ROOT/src.
#   build-gl-deps.sh
set -euo pipefail
BASE=/mnt/HaikuWork/build/summit-arm64
ROOT=${SUMMIT_GL_ROOT:-/mnt/HaikuWork/rpi4/summit-gl}
P=$ROOT/deps
B=$ROOT/depbuild
SRC=$ROOT/src
SYSROOT=$BASE/sysroot
X=/mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-
J=${JOBS:-12}
. $BASE/hosttools/env.sh
mkdir -p "$P/lib/pkgconfig" "$B/.stamps" "$SRC"

# The toolchain file of the base build, with this prefix searched first.
TC=$ROOT/haiku-arm64-gl.cmake
sed -e "s|set(CMAKE_FIND_ROOT_PATH |set(CMAKE_FIND_ROOT_PATH $P |" \
    -e "s|set(CMAKE_PREFIX_PATH |set(CMAKE_PREFIX_PATH $P |" \
    -e "s|-I$BASE/deps/include|-I$P/include -I$BASE/deps/include|g" \
    -e "s|-L$BASE/deps/lib -Wl,-rpath-link,$BASE/deps/lib|-L$P/lib -Wl,-rpath-link,$P/lib -L$BASE/deps/lib -Wl,-rpath-link,$BASE/deps/lib|g" \
    -e "s|\"$BASE/deps/lib/pkgconfig\"|\"$P/lib/pkgconfig:$BASE/deps/lib/pkgconfig\"|" \
    $BASE/haiku-arm64.cmake > "$TC"

fetch() { # url
    local file=$SRC/$(basename "$1")
    [ -s "$file" ] || curl -sfL --retry 3 -o "$file" "$1"
    local dir=$SRC/$(basename "$file" .tar.xz)
    [ -d "$dir" ] || tar -C "$SRC" -xf "$file"
}
fetch https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz
fetch https://github.com/libexpat/libexpat/releases/download/R_2_6_4/expat-2.6.4.tar.xz
fetch https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.15.0.tar.xz
fetch https://github.com/harfbuzz/harfbuzz/releases/download/10.1.0/harfbuzz-10.1.0.tar.xz
fetch https://download.gnome.org/sources/libepoxy/1.5/libepoxy-1.5.10.tar.xz

step() { local name=$1; shift
    [ -e "$B/.stamps/$name" ] && { echo "== $name (done)"; return; }
    echo "== $name"
    ( "$@" ) > "$B/$name.log" 2>&1 || { echo "FAILED $name, see $B/$name.log"; tail -30 "$B/$name.log"; exit 1; }
    touch "$B/.stamps/$name"; }

cm() { # source dir, extra cmake arguments
    local d=$1; shift
    rm -rf "$B/$d"
    cmake -S "$SRC/$d" -B "$B/$d" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
        -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@"
    ninja -C "$B/$d" -j"$J"
    ninja -C "$B/$d" install; }

freetype() { cm freetype-2.13.3 -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON \
    -DFT_DISABLE_BZIP2=ON -DFT_REQUIRE_PNG=ON -DFT_REQUIRE_ZLIB=ON; }

expat() { cm expat-2.6.4 -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF \
    -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF; }

fontconfig() {
    rm -rf "$B/fontconfig-2.15.0"; mkdir -p "$B/fontconfig-2.15.0"; cd "$B/fontconfig-2.15.0"
    # Fonts are where Haiku keeps them; the cache goes to the user's settings.
    CC="${X}gcc --sysroot=$SYSROOT" AR=${X}ar RANLIB=${X}ranlib STRIP=${X}strip \
    CFLAGS="-O2 -fPIC -I$P/include -I$BASE/deps/include" \
    LDFLAGS="-L$P/lib -Wl,-rpath-link,$P/lib -L$BASE/deps/lib -Wl,-rpath-link,$BASE/deps/lib -Wl,-rpath-link,$SYSROOT/boot/system/lib" \
    PKG_CONFIG_LIBDIR=$P/lib/pkgconfig:$BASE/deps/lib/pkgconfig PKG_CONFIG_PATH= \
    "$SRC/fontconfig-2.15.0/configure" --host=aarch64-unknown-haiku --prefix="$P" \
        --enable-shared --disable-static --disable-docs --disable-nls \
        --disable-cache-build \
        --sysconfdir=/boot/system/lib/summit-webkit/etc \
        --localstatedir=/boot/system/var \
        --with-default-fonts=/boot/system/data/fonts \
        --with-add-fonts=/boot/system/non-packaged/data/fonts,/boot/home/config/data/fonts,/boot/home/config/non-packaged/data/fonts \
        --with-cache-dir=/boot/home/config/cache/fontconfig
    make -j"$J"
    # The configuration goes into the prefix (the package puts it where
    # sysconfdir says), and the cache directory is the board's, not ours.
    make install sysconfdir="$P/etc" fc_cachedir="$B/unused-fontconfig-cache"
}

harfbuzz() { cm harfbuzz-10.1.0 -DHB_HAVE_FREETYPE=ON -DHB_HAVE_ICU=ON \
    -DHB_BUILD_UTILS=OFF -DHB_BUILD_SUBSET=OFF -DHB_HAVE_GLIB=OFF; }

epoxy() {
    # meson, with the cross compiler described the way the toolchain file does
    local cross=$B/haiku-aarch64-gl.ini
    cat > "$cross" <<INI
[binaries]
c = '${X}gcc'
cpp = '${X}g++'
ar = '${X}ar'
strip = '${X}strip'
pkg-config = '/usr/bin/pkg-config'

[host_machine]
system = 'haiku'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true
sys_root = '$SYSROOT'
pkg_config_libdir = ['$P/lib/pkgconfig', '$BASE/deps/lib/pkgconfig']

[built-in options]
c_args = ['--sysroot=$SYSROOT', '-I$SYSROOT/boot/system/develop/headers/os/opengl']
c_link_args = ['--sysroot=$SYSROOT']
INI
    rm -rf "$B/libepoxy-1.5.10"
    PATH=/mnt/HaikuWork/toolchains/mesa-python/bin:$PATH \
    meson setup "$B/libepoxy-1.5.10" "$SRC/libepoxy-1.5.10" --cross-file="$cross" \
        --prefix="$P" --libdir=lib --buildtype=release -Degl=yes -Dglx=no \
        -Dx11=false -Dtests=false -Ddocs=false
    ninja -C "$B/libepoxy-1.5.10" -j"$J"
    ninja -C "$B/libepoxy-1.5.10" install
}

step freetype freetype
step expat expat
step fontconfig fontconfig
step harfbuzz harfbuzz
step epoxy epoxy
echo "GL dependencies -> $P (toolchain file $TC)"
