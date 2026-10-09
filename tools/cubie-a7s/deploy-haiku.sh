#!/usr/bin/env bash
# Put a Haiku build on the Cubie A7S lab card through its Debian recovery
# system, and have U-Boot boot it once (airos/haiku-once on the ESP).
#
#   deploy-haiku.sh [-n] [bfs image] [haiku_loader.efi] [device tree]
#
#   -n   copy only, do not restart into Haiku
#
# The defaults are the build directory's haiku-cubie-minimum.image and EFI
# loader and tools/cubie-a7s/build-dtb.sh's device tree. The BFS volume goes
# to the partition labelled "haiku", the loader to EFI/airos/haiku_loader.efi
# and the device tree to airos/cubie-a7s.dtb on the ESP; all are read back
# and compared.
# $CUBIE_STATE/ssh_config names the recovery system (host cubie-recovery).
set -euo pipefail

restart=1
if [[ ${1:-} == -n ]]; then
	restart=0
	shift
fi
BUILD=${CUBIE_BUILD:-/mnt/HaikuWork/cubie/build}
IMAGE=${1:-$BUILD/haiku-cubie-minimum.image}
LOADER=${2:-$BUILD/objects/haiku/arm64/release/system/boot/efi/haiku_loader.efi}
DTB=${3:-$BUILD/cubie-a7s.dtb}
if [[ -z ${3:-} ]]; then
	"$(dirname "${BASH_SOURCE[0]}")/build-dtb.sh" "$DTB" >/dev/null
fi
CUBIE_STATE=${CUBIE_STATE:-/mnt/HaikuWork/cubie/state}
SSH=(ssh -F "$CUBIE_STATE/ssh_config" cubie-recovery)

[[ -f $IMAGE && -f $LOADER ]] || { echo "missing $IMAGE or $LOADER" >&2; exit 1; }

size=$(stat -c %s "$IMAGE")
partSize=$("${SSH[@]}" 'blockdev --getsize64 /dev/disk/by-partlabel/haiku')
if (( size > partSize )); then
	echo "the image ($size bytes) is larger than the partition ($partSize)" >&2
	exit 1
fi

echo "Writing $(basename "$IMAGE") ($((size / 1048576)) MiB)"
start=$SECONDS
zstd -q -T0 -3 -c "$IMAGE" | "${SSH[@]}" \
	'zstd -q -d -c | dd of=/dev/disk/by-partlabel/haiku bs=4M iflag=fullblock conv=fsync status=none'
echo "  written in $((SECONDS - start)) s, comparing"
want=$(sha256sum < "$IMAGE" | cut -d' ' -f1)
have=$("${SSH[@]}" "head -c $size /dev/disk/by-partlabel/haiku | sha256sum" | cut -d' ' -f1)
if [[ $want != "$have" ]]; then
	echo "read back $have, expected $want" >&2
	exit 1
fi

echo "Copying the loader and the device tree to the ESP"
"${SSH[@]}" 'set -e; mkdir -p /mnt/esp; mountpoint -q /mnt/esp || mount /dev/disk/by-partlabel/efi /mnt/esp; mkdir -p /mnt/esp/EFI/airos /mnt/esp/airos'
put_esp() { # <local file> <path on the ESP>
	"${SSH[@]}" "cat > '/mnt/esp/$2.new'" < "$1"
	local want have
	want=$(sha256sum < "$1" | cut -d' ' -f1)
	have=$("${SSH[@]}" "sha256sum < '/mnt/esp/$2.new'" | cut -d' ' -f1)
	if [[ $want != "$have" ]]; then
		echo "$2 read back $have, expected $want" >&2
		exit 1
	fi
	"${SSH[@]}" "mv '/mnt/esp/$2.new' '/mnt/esp/$2' && sync"
}
put_esp "$LOADER" EFI/airos/haiku_loader.efi
put_esp "$DTB" airos/cubie-a7s.dtb

if (( restart )); then
	echo "Restarting into Haiku (once)"
	"${SSH[@]}" 'touch /mnt/esp/airos/haiku-once && sync && umount /mnt/esp; systemctl reboot' || true
fi
