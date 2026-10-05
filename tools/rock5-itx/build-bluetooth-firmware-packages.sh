#!/usr/bin/env bash
# Package Bluetooth firmware from a linux-firmware tree as architecture-neutral
# Haiku packages, the Bluetooth counterparts of HaikuPorts' *_wifi_firmwares:
#
#   intel_bluetooth_firmwares     intel/ibt-*         -> data/firmware/intel
#       (loaded by bt_firmware: Wireless 7260 to Wi-Fi 7 BE200)
#   realtek_bluetooth_firmwares   rtl_bt/*            -> data/firmware/rtl_bt
#       (loaded by bt_firmware: RTL8723, 8761, 8821, 8822, 8851, 8852, 8922)
#   mediatek_bluetooth_firmwares  mediatek/BT_RAM_*   -> data/firmware/h2generic
#       (loaded by h2generic's h2mediatek: MT7921, MT7922)
#
# Firmware stays outside the repository; the packages go to
# rock5-image-extras/packages.
#
# usage: build-bluetooth-firmware-packages.sh [LINUX_FIRMWARE_DIR [LICENSE_DIR]]
# defaults: /lib/firmware and the Debian/Ubuntu linux-firmware licence copies.
set -euo pipefail
source "$(dirname "$0")/env.sh"

firmware=${1:-/lib/firmware}
licenses=${2:-/usr/share/doc/linux-firmware/licenses}
tool="$HAIKU_WORK/build/arm64/objects/linux/x86_64/release/tools/package/package"
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

strip_suffix()
{
	local name=${1%.zst}
	name=${name%.xz}
	printf '%s' "$name"
}

# build_package NAME DESTINATION LICENSE_FILE LICENSE_NAME COPYRIGHT
#	SUMMARY DESCRIPTION SOURCE...
build_package()
{
	local name=$1 destinationDir=$2 licenseFile=$3 licenseName=$4
	local copyright=$5 summary=$6 description=$7
	shift 7

	local stage
	stage=$(mktemp -d "$HAIKU_WORK/tmp/bt-firmware-XXXXXX")
	local destination="$stage/data/firmware/$destinationDir"
	mkdir -p "$destination" "$stage/data/licenses"

	# Regular files are decompressed; symlinks (many Intel names are
	# aliases of one image) are recreated without the compression suffix.
	local count=0 source plain target
	for source in "$@"; do
		[ -e "$source" ] || continue
		plain=$(strip_suffix "$(basename "$source")")
		if [ -L "$source" ]; then
			target=$(strip_suffix "$(readlink "$source")")
			ln -s "$target" "$destination/$plain"
		else
			decompress "$source" > "$destination/$plain"
			chmod 0444 "$destination/$plain"
		fi
		count=$((count + 1))
	done
	[ "$count" -gt 0 ] || { echo "$name: no firmware files" >&2; exit 1; }
	for target in "$destination"/*; do
		[ -e "$target" ] || { echo "Dangling link: $target" >&2; exit 1; }
	done
	decompress "$licenseFile" > "$stage/data/licenses/$licenseName"

	cat > "$stage/.PackageInfo" <<EOF
name			$name
version			${version}-1
architecture		any
summary			"$summary"
description		"$description (linux-firmware $version)"
packager		"air/OS <noreply@haiku-os.org>"
vendor			"Haiku Project"
licenses {
	"$licenseName"
}
copyrights {
	"$copyright"
}
provides {
	$name = $version
}
urls {
	"https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git"
}
EOF

	local output="$HAIKU_WORK/rock5-image-extras/packages/$name-${version}-1-any.hpkg"
	mkdir -p "$(dirname "$output")"
	rm -f -- "$output"
	(cd "$stage" && "$tool" create -q "$output")
	rm -rf -- "$stage"
	echo "$output: $count files"
	sha256sum "$output"
}

build_package intel_bluetooth_firmwares intel \
	"$licenses/LICENCE.ibt_firmware.gz" "Intel Bluetooth Firmware" \
	"2014-2024 Intel Corporation" \
	"Intel Bluetooth firmware" \
	"Firmware for the Bluetooth half of Intel Wi-Fi cards, Wireless 7260 to Wi-Fi 7 BE200, loaded by bt_firmware" \
	"$firmware"/intel/ibt-*

build_package realtek_bluetooth_firmwares rtl_bt \
	"$licenses/LICENCE.rtlwifi_firmware.txt.gz" "Realtek Bluetooth Firmware" \
	"Realtek Semiconductor Corp." \
	"Realtek Bluetooth firmware" \
	"Firmware and configuration for Realtek USB Bluetooth controllers and the Bluetooth half of Realtek Wi-Fi cards (RTL8723 to RTL8922), loaded by bt_firmware" \
	"$firmware"/rtl_bt/*

build_package mediatek_bluetooth_firmwares h2generic \
	"$licenses/LICENCE.mediatek.gz" "MediaTek Firmware" \
	"MediaTek Inc." \
	"MediaTek Bluetooth firmware" \
	"Firmware for the Bluetooth half of MediaTek MT7921 and MT7922 Wi-Fi cards, loaded by the h2generic driver" \
	"$firmware"/mediatek/BT_RAM_CODE_MT79*
