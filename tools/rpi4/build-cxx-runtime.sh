#!/usr/bin/env bash
# Rebuild the pinned GCC 13.3 C++ runtime with SysV and GNU symbol hashes.
# The SysV comparison build, ABI checks and manifest stay beside the stage.
set -euo pipefail
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$TOOLS/../rock5-itx/env.sh"
ROOT=${1:-/mnt/HaikuWork/rpi4/cxx-runtime}
CROSS=/mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-
SOURCE=/mnt/HaikuWork/src/buildtools
CXX_SYSROOT=/mnt/HaikuWork/artifacts/mali-system-opengl-build/20260918T125722Z/sysroot
PACKAGES=/mnt/HaikuWork/rpi4/build/build_packages
NATIVE_GCC=$PACKAGES/gcc_syslibs_devel-13.3.0_2026_03_29_bootstrap-1-arm64
NATIVE_RUNTIME=$PACKAGES/gcc_syslibs-13.3.0_2026_03_29_bootstrap-1-arm64
REVISION=8375c2dbeaf109c520798cb234d57f0895463201
test "$(git -C "$SOURCE" rev-parse HEAD)" = "$REVISION"
git -C "$SOURCE" diff --quiet HEAD -- gcc/libstdc++-v3 gcc/libgcc gcc/gcc/BASE-VER
test "$(cat "$SOURCE/gcc/gcc/BASE-VER")" = 13.3.0
mkdir -p "$ROOT/build" "$ROOT/libgcc" "$ROOT/stage" "$ROOT/sysv" "$ROOT/both"
ROOT=$(cd "$ROOT" && pwd)
ln -sfn "$SOURCE/gcc/libgcc/gthr-posix.h" "$ROOT/libgcc/gthr-default.h"
export CC="${CROSS}gcc --sysroot=$CXX_SYSROOT"
export CXX="${CROSS}g++ --sysroot=$CXX_SYSROOT"
export AR="${CROSS}ar" RANLIB="${CROSS}ranlib" LD="${CROSS}ld" NM="${CROSS}nm"
export CFLAGS='-O2 -g' CXXFLAGS='-O2 -g' LDFLAGS='-Wl,--hash-style=both'
# Preserve the installed runtime's feature configuration. Newer Haiku headers
# expose optional clock APIs; enabling them is outside this link-table change.
export glibcxx_cv_PTHREAD_COND_CLOCKWAIT=no
export glibcxx_cv_PTHREAD_MUTEX_CLOCKLOCK=no
export glibcxx_cv_PTHREAD_RWLOCK_CLOCKLOCK=no
cd "$ROOT/build"
"$SOURCE/gcc/libstdc++-v3/configure" \
    --build=x86_64-pc-linux-gnu --host=aarch64-unknown-haiku \
    --target=aarch64-unknown-haiku --with-cross-host=x86_64-pc-linux-gnu \
    --prefix=/boot/system --enable-shared --disable-static \
    --disable-multilib --disable-nls --disable-maintainer-mode \
    --disable-libstdcxx-pch --enable-libstdcxx-threads \
    --with-default-libstdcxx-abi=gcc4-compatible > "$ROOT/configure.log" 2>&1
make clean > "$ROOT/clean.log" 2>&1
make -j"${RPI4_CXX_JOBS:-8}" > "$ROOT/build.log" 2>&1
python3 - "$ROOT" "$CROSS" "$NATIVE_GCC" "$NATIVE_RUNTIME" "$REVISION" <<'PY'
from pathlib import Path
import hashlib, json, shlex, shutil, subprocess, sys
root, cross, gcc, runtime, revision = sys.argv[1:]
root, gcc, runtime = Path(root), Path(gcc), Path(runtime)
original = runtime/'lib/libstdc++.so.6.0.32'
support = gcc/'develop/lib/libgcc.a'
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
assert sha(original) == 'f3a7df4507cf976d45daa8af613958e9151a1cef5d80e492d7ea922d3796f871'
assert sha(support) == '54ec0f159e84a2f8d35a1de4badf7456738d11c70abacc30b239ac1f46f3d94d'
header = gcc/'develop/headers/c++/aarch64-unknown-haiku/bits/c++config.h'
assert header.read_bytes() == (root/'build/include/aarch64-unknown-haiku/bits/c++config.h').read_bytes()
lines = [line.removeprefix('libtool: link: ') for line in (root/'build.log').read_text().splitlines()
    if line.startswith('libtool: link: '+cross+'g++') and '-shared' in line
    and '-o .libs/libstdc++.so.6.0.32' in line]
assert len(lines) == 1
args = shlex.split(lines[0])
# Avoid g++ adding its installed libstdc++ as a self-dependency. The packaged
# support archive retains the original imports from the shared unwinder.
args[0] = cross+'gcc'
args = [str(support) if arg == '-lgcc' else arg for arg in args]
args.insert(1, '-nodefaultlibs')
def symbols(path, kind):
    output = subprocess.check_output([cross+'nm', '-D', kind, '--format=posix', str(path)], text=True)
    return {line.split()[0]: line.split()[1] for line in output.splitlines()}
def dynamic(path):
    return subprocess.check_output([cross+'readelf', '-d', str(path)], text=True)
manifest = {'source_revision': revision, 'compiler': subprocess.check_output([cross+'g++', '--version'], text=True).splitlines()[0],
    'original_sha256': sha(original), 'native_libgcc_sha256': sha(support), 'variants': {}}
for style, directory in [('sysv', root/'sysv'), ('both', root/'both')]:
    output = directory/'libstdc++.so.6'
    command = [arg.replace('--hash-style=both', '--hash-style='+style) for arg in args]
    command[command.index('-o')+1] = str(output)
    with (root/('link-'+style+'.log')).open('w') as log:
        subprocess.run(command, cwd=root/'build/src', stdout=log, stderr=subprocess.STDOUT, check=True)
    for kind in ['--defined-only', '--undefined-only']:
        assert symbols(output, kind) == symbols(original, kind), (style, kind)
    metadata = dynamic(output)
    needed = [line for line in metadata.splitlines() if '(NEEDED)' in line]
    assert len(needed) == 1 and '[libroot.so]' in needed[0], needed
    assert '(HASH)' in metadata and ('(GNU_HASH)' in metadata) == (style == 'both')
    assert 'Library soname: [libstdc++.so.6]' in metadata
    manifest['variants'][style] = {'sha256': sha(output), 'size': output.stat().st_size,
        'defined_symbols': len(symbols(output, '--defined-only')),
        'imported_symbols': len(symbols(output, '--undefined-only'))}
# Publish only after both link variants pass every compatibility check.
staged = root/'stage/libstdc++.so.6.new'
shutil.copyfile(root/'both/libstdc++.so.6', staged)
staged.replace(root/'stage/libstdc++.so.6')
(root/'manifest.json.new').write_text(json.dumps(manifest, indent=2)+'\n')
(root/'manifest.json.new').replace(root/'manifest.json')
print(json.dumps(manifest, indent=2))
PY
