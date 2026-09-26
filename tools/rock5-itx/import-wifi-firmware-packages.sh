#!/usr/bin/env bash
# Copy HaikuPorts' architecture-neutral Wi-Fi firmware packages into
# rock5-image-extras/packages for the arm64 image. The arm64 build has no zstd
# build feature, so packagefs cannot read zstd packages ("failed to init
# package") and the drivers would find no firmware; such packages are
# recompressed with zlib. Name and version stay the same.
#
# usage: import-wifi-firmware-packages.sh DOWNLOAD_DIR
#   DOWNLOAD_DIR: an x86_64 build's download directory holding
#   intel_wifi_firmwares, ralink_wifi_firmwares and realtek_wifi_firmwares.
set -euo pipefail
source "$(dirname "$0")/env.sh"

[ "$#" -eq 1 ] || { echo "usage: $0 DOWNLOAD_DIR" >&2; exit 2; }
tool="$HAIKU_WORK/build/arm64/objects/linux/x86_64/release/tools/package/package"
[ -x "$tool" ] || { echo "Missing Haiku package tool: $tool" >&2; exit 1; }
destination="$HAIKU_WORK/rock5-image-extras/packages"
mkdir -p "$destination"

# Heap compression is the 16-bit field at offset 18 of the hpkg header:
# 0 none, 1 zlib, 2 zstd.
compression()
{
	od -An -tu1 -j18 -N2 -- "$1" | awk '{print $1 * 256 + $2}'
}

for name in intel_wifi_firmwares-2025_02_11-1-any.hpkg \
		ralink_wifi_firmwares-2023_08_04-1-any.hpkg \
		realtek_wifi_firmwares-2019_01_02-1-any.hpkg; do
	source="$1/$name"
	[ -s "$source" ] || { echo "Missing $source" >&2; exit 1; }
	if [ "$(compression "$source")" = 2 ]; then
		"$tool" recompress -q -z zlib "$source" "$destination/$name"
	else
		install -m 0644 "$source" "$destination/$name"
	fi
	[ "$(compression "$destination/$name")" != 2 ] \
		|| { echo "$name is still zstd" >&2; exit 1; }
	sha256sum "$destination/$name"
done
