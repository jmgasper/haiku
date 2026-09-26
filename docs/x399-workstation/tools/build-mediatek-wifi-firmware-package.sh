#!/usr/bin/env bash
# Package the MediaTek MT7921/MT7922 Wi-Fi firmware from a linux-firmware tree
# as an architecture-neutral Haiku package, the Wi-Fi counterpart of rock5-itx's
# mediatek_bluetooth_firmwares:
#
#   mediatek_wifi_firmwares   mediatek/WIFI_*MT79{21,22,61}*
#                                 -> data/firmware/mt7922wifi
#       (loaded by the mt7922wifi driver through firmware_get)
#
# The firmware is MediaTek's, redistributed by linux-firmware; it stays out of
# the repository. The package goes to x399/pkgs/firmware.
#
# usage: build-mediatek-wifi-firmware-package.sh [LINUX_FIRMWARE_DIR [LICENSE_DIR]]
# defaults: /lib/firmware and the Debian/Ubuntu linux-firmware licence copies.
set -euo pipefail

X399=/mnt/HaikuWork/x399
firmware=${1:-/lib/firmware}
licenses=${2:-/usr/share/doc/linux-firmware/licenses}
tool=$X399/build/x86_64/objects/linux/x86_64/release/tools/package/package
out=$X399/pkgs/firmware
[ -x "$tool" ] || { echo "Missing Haiku package tool: $tool" >&2; exit 1; }

# The linux-firmware release date is the package version.
version=
if command -v dpkg-query >/dev/null 2>&1; then
	version=$(dpkg-query -W -f '${Version}' linux-firmware 2>/dev/null \
		| sed -n 's/^\([0-9]\{8\}\).*/\1/p')
fi
version=${version:-$(date -u +%Y%m%d)}

decompress()
{
	case "$1" in
		*.zst) zstd -qdc -- "$1" ;;
		*.xz) xz -dc -- "$1" ;;
		*.gz) gzip -dc -- "$1" ;;
		*) cat -- "$1" ;;
	esac
}

name=mediatek_wifi_firmwares
stage=$(mktemp -d /mnt/HaikuWork/tmp/mtk-wifi-firmware-XXXXXX)
trap 'rm -rf -- "$stage"' EXIT
destination=$stage/data/firmware/mt7922wifi
mkdir -p "$destination" "$stage/data/licenses"

count=0
for source in "$firmware"/mediatek/WIFI_MT7922_* "$firmware"/mediatek/WIFI_RAM_CODE_MT7922_* \
		"$firmware"/mediatek/WIFI_MT7961_* "$firmware"/mediatek/WIFI_RAM_CODE_MT7961_*; do
	[ -e "$source" ] || continue
	plain=$(basename "$source")
	plain=${plain%.zst}; plain=${plain%.xz}
	decompress "$source" > "$destination/$plain"
	chmod 0444 "$destination/$plain"
	count=$((count + 1))
done
[ "$count" -gt 0 ] || { echo "$name: no firmware files" >&2; exit 1; }
decompress "$licenses/LICENCE.mediatek.gz" > "$stage/data/licenses/MediaTek Firmware"

cat > "$stage/.PackageInfo" <<EOF
name			$name
version			${version}-1
architecture		any
summary			"MediaTek Wi-Fi firmware"
description		"Firmware for the Wi-Fi half of MediaTek MT7921 and MT7922 cards (such as the TP-Link Archer TX55E), loaded by the mt7922wifi driver (linux-firmware $version)"
packager		"x399-workstation <noreply@haiku-os.org>"
vendor			"Haiku Project"
licenses {
	"MediaTek Firmware"
}
copyrights {
	"MediaTek Inc."
}
provides {
	$name = $version
}
urls {
	"https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git"
}
EOF

mkdir -p "$out"
output=$out/$name-${version}-1-any.hpkg
rm -f -- "$output"
(cd "$stage" && "$tool" create -q "$output")
echo "$output: $count files"
sha256sum "$output"
