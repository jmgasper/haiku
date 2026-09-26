#!/usr/bin/env bash
# Stage the redistributable AX210 firmware from HaikuPorts' architecture-neutral
# Intel firmware package. The package itself stays outside the source tree.
set -euo pipefail
source "$(dirname "$0")/env.sh"

if [ "$#" -ne 1 ]; then
	echo "usage: $0 intel_wifi_firmwares-2025_02_11-1-any.hpkg" >&2
	exit 2
fi

package=$1
expected=8a8a5cb0aeed4f3ad2cb95b87e980dd85b143c7382985127606d3abacef637a1
actual=$(sha256sum "$package" | awk '{print $1}')
[ "$actual" = "$expected" ] || {
	echo "Intel firmware package checksum mismatch: $actual" >&2
	exit 1
}

tool="$HAIKU_WORK/build/arm64/objects/linux/x86_64/release/tools/package/package"
[ -x "$tool" ] || { echo "Missing Haiku package tool: $tool" >&2; exit 1; }
stage=$(mktemp -d "$HAIKU_WORK/tmp/ax210-firmware-XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
"$tool" extract -C "$stage" "$package" data/firmware/iaxwifi200 \
	data/licenses >/dev/null

destination="$HAIKU_WORK/rock5-image-extras/firmware/iaxwifi200"
licenses="$HAIKU_WORK/rock5-image-extras/licenses"
mkdir -p "$destination" "$licenses"
install -m 0444 "$stage/data/firmware/iaxwifi200/iwx-ty-a0-gf-a0-77" \
	"$destination/"
install -m 0444 "$stage/data/firmware/iaxwifi200/iwx-ty-a0-gf-a0.pnvm" \
	"$destination/"
install -m 0444 "$stage/data/licenses/Intel WiFi Firmware" "$licenses/"

sha256sum "$destination/iwx-ty-a0-gf-a0-77" \
	"$destination/iwx-ty-a0-gf-a0.pnvm" "$licenses/Intel WiFi Firmware"
