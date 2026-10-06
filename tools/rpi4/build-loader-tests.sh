#!/usr/bin/env bash
# Cross-build the existing runtime-loader tests with SysV and dual hashes.
# Extract the resulting hpkg into a private directory on Haiku, then run
# `sh run-tests.sh` there. It need not be installed as a system package.
set -euo pipefail
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SOURCE=$(cd "$TOOLS/../.." && pwd)
ROOT=${1:-/mnt/HaikuWork/rpi4/build/loader-tests}
export HAIKU_TEST_ROOT=$ROOT/files
export HAIKU_TEST_CC=/mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-gcc
export HAIKU_TEST_SYSROOT=/mnt/HaikuWork/artifacts/mali-system-opengl-build/20260918T125722Z/sysroot
mkdir -p "$ROOT/source" "$HAIKU_TEST_ROOT"
cp "$SOURCE"/src/tests/system/runtime_loader/test_suite/{load_*,dlopen_*} "$ROOT/source/"
: > "$HAIKU_TEST_ROOT/expected.tsv"
cat > "$ROOT/source/test_setup" <<'SETUP'
set -eu
os=Haiku
testname=$(basename "$0")
mkdir -p "$HAIKU_TEST_ROOT/$HAIKU_HASH_STYLE/$testname"
cd "$HAIKU_TEST_ROOT/$HAIKU_HASH_STYLE/$testname"
test_run_ok() {
    test -x "$1"
    printf '%s\t%s\t%s\n' "$HAIKU_HASH_STYLE/$testname" "$1" "$2" >> "$HAIKU_TEST_ROOT/expected.tsv"
}
compile_lib() {
    "$HAIKU_TEST_CC" --sysroot="$HAIKU_TEST_SYSROOT" -shared -Wl,--no-as-needed -D_GNU_SOURCE -fPIC -Wl,--hash-style="$HAIKU_HASH_STYLE" "$@"
}
compile_lib_dl() { compile_lib "$@"; }
compile_program() {
    "$HAIKU_TEST_CC" --sysroot="$HAIKU_TEST_SYSROOT" -Wl,--no-as-needed -D_GNU_SOURCE -Wl,-rpath,.,--export-dynamic -Wl,--hash-style="$HAIKU_HASH_STYLE" "$@"
}
compile_program_dl() { compile_program "$@"; }
SETUP
for HAIKU_HASH_STYLE in sysv both; do
    export HAIKU_HASH_STYLE
    for test in "$ROOT"/source/{load_*,dlopen_*}; do
        (cd "$ROOT/source"; sh "./$(basename "$test")")
    done
done
cat > "$HAIKU_TEST_ROOT/run-tests.sh" <<'RUN'
#!/bin/sh
cd "$(dirname "$0")" || exit 1
root=$PWD
failures=0
count=0
while IFS="$(printf '\t')" read -r directory program expected; do
    (cd "$root/$directory" && LIBRARY_PATH=.:/boot/system/non-packaged/lib:/boot/system/lib timeout 10 "$program")
    actual=$?
    count=$((count + 1))
    if [ "$actual" = "$expected" ]; then
        echo "PASS $directory ($actual)"
    else
        echo "FAIL $directory: expected $expected, got $actual"
        failures=$((failures + 1))
    fi
done < expected.tsv
echo "$count tests, $failures failures"
test "$failures" = 0
RUN
cat > "$HAIKU_TEST_ROOT/.PackageInfo" <<'INFO'
name rpi4_loader_tests
version 1.0-1
architecture arm64
summary "Runtime loader regression checks for the Raspberry Pi lab"
description "Existing Haiku loader tests built with SysV and dual symbol hashes"
packager "air/OS"
vendor "air/OS"
copyrights { "Haiku contributors" }
licenses { "MIT" }
provides { rpi4_loader_tests = 1.0 }
INFO
/mnt/HaikuWork/rpi4/build/objects/linux/x86_64/release/tools/package/package \
    create -q -C "$HAIKU_TEST_ROOT" "$ROOT/rpi4_loader_tests.hpkg"
echo "$ROOT/rpi4_loader_tests.hpkg"
