#!/usr/bin/env bash
# Local QEMU for user-instantiated eMMC; leave the system emulator in place.
set -euo pipefail
umask 077
source "$(dirname "$0")/env.sh"
exec 9>"$HAIKU_WORK/state/qemu-mmc-build.lock"
flock -n 9 || { echo 'Another build owns the MMC emulator lock.' >&2; exit 1; }

mkdir -p "$HAIKU_WORK/cache/debs" "$HAIKU_WORK/cache/qemu" "$HAIKU_WORK/build/qemu-10.2.0"
if ! command -v ninja >/dev/null; then
    (
        cd "$HAIKU_WORK/cache/debs"
        apt-get download ninja-build=1.11.1-2
        dpkg-deb -x ninja-build_1.11.1-2_amd64.deb "$HAIKU_WORK/toolchains/host"
    )
fi
export PKG_CONFIG_PATH="$HAIKU_WORK/toolchains/qemu-host/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if ! pkg-config --exists slirp; then
    (
        cd "$HAIKU_WORK/cache/debs"
        apt-get download libslirp-dev=4.7.0-1ubuntu3.1
        dpkg-deb -x libslirp-dev_4.7.0-1ubuntu3.1_amd64.deb "$HAIKU_WORK/toolchains/qemu-host"
    )
    python3 - <<'PY'
import os
from pathlib import Path
root = Path(os.environ['HAIKU_WORK']) / 'toolchains/qemu-host/usr'
pc = root / 'lib/x86_64-linux-gnu/pkgconfig/slirp.pc'
pc.write_text(pc.read_text().replace('prefix=/usr', 'prefix=' + str(root)))
PY
fi
pkg-config --exists glib-2.0 pixman-1 slirp
python3 - <<'PY'
import hashlib
import os
from pathlib import Path
import tarfile
import urllib.request
work = Path(os.environ['HAIKU_WORK'])
archive = work / 'cache/qemu/qemu-10.2.0.tar.xz'
if not archive.exists():
    temporary = archive.with_suffix('.download')
    with urllib.request.urlopen('https://download.qemu.org/qemu-10.2.0.tar.xz', timeout=30) as response:
        with temporary.open('wb') as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
    temporary.rename(archive)
with archive.open('rb') as stream:
    digest = hashlib.file_digest(stream, 'sha256').hexdigest()
if digest != '9e30ad1b8b9f7b4463001582d1ab297f39cfccea5d08540c0ca6d6672785883a':
    raise RuntimeError('QEMU source checksum mismatch')
if not (work / 'src/qemu-10.2.0/configure').exists():
    with tarfile.open(archive) as source:
        # The packaged macOS X11 convenience link is unused by this build.
        members = [member for member in source.getmembers() if member.name !=
                   'qemu-10.2.0/roms/edk2/EmulatorPkg/Unix/Host/X11IncludeHack']
        source.extractall(work / 'src', members=members, filter='data')
PY

cd "$HAIKU_WORK/build/qemu-10.2.0"
if [[ ! -f build.ninja ]]; then
    "$HAIKU_WORK/src/qemu-10.2.0/configure" --target-list=aarch64-softmmu \
        --prefix="$HAIKU_WORK/toolchains/qemu-10.2.0" --disable-docs --disable-guest-agent \
        --disable-user --disable-tools --enable-slirp --disable-werror \
        >"$HAIKU_WORK/tmp/qemu-emmc-10.2-configure.log" 2>&1
fi
log="$HAIKU_WORK/artifacts/qemu-mmc-build-$(date -u +%Y%m%dT%H%M%SZ).log"
ninja -j"$HAIKU_JOBS" qemu-system-aarch64 >"$log" 2>&1
python3 - "$log" <<'PY'
import os
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(os.environ['HAIKU_SOURCE']) / 'tools/rock5-itx'))
import lab
binary = lab.WORK / 'build/qemu-10.2.0/qemu-system-aarch64'
archive = lab.WORK / 'cache/qemu/qemu-10.2.0.tar.xz'
devices = subprocess.check_output([str(binary), '-device', 'help'], text=True)
if 'name "emmc"' not in devices or 'name "sdhci-pci"' not in devices:
    raise RuntimeError('Required user-creatable MMC models are missing')
manifest = dict(binary=str(binary), binary_sha256=lab.digest(binary), source=str(archive),
    source_url='https://download.qemu.org/qemu-10.2.0.tar.xz', source_sha256=lab.digest(archive),
    version=subprocess.check_output([str(binary), '--version'], text=True).splitlines()[0],
    configure_log=str(lab.WORK / 'tmp/qemu-emmc-10.2-configure.log'), build_log=sys.argv[1],
    host_dependencies=subprocess.check_output(['pkg-config', '--modversion', 'glib-2.0', 'pixman-1', 'slirp'], text=True).splitlines(),
    dtc_revision=subprocess.check_output(['git', '-C', str(lab.WORK / 'src/qemu-10.2.0/subprojects/dtc'), 'rev-parse', 'HEAD'], text=True).strip())
lab.save(lab.WORK / 'state/qemu-mmc-toolchain.json', manifest)
print(manifest['version'], manifest['binary_sha256'])
PY
