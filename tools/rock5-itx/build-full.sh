#!/usr/bin/env bash
# Build and record the regular ROCK 5 ITX arm64 image and its local inputs.
set -euo pipefail
source "$(dirname "$0")/env.sh"
exec 9>"$HAIKU_WORK/state/build.lock"
flock -n 9 || { echo "Another build owns the build lock." >&2; exit 1; }

for needed in \
    "$HAIKU_WORK/rock5-image-extras/develop/fluidlite/lib/libfluidlite.a" \
    "$HAIKU_WORK/rock5-image-extras/packages/rock5_glinfo-1.0.0-1-arm64.hpkg" \
    "$HAIKU_WORK/rock5-image-extras/packages/rock5_ffmpeg-6.1.6-2-arm64.hpkg" \
    "$HAIKU_WORK/rock5-image-extras/packages/wpa_supplicant-2.11.haiku.1-1-arm64.hpkg" \
    "$HAIKU_WORK/rock5-image-extras/packages/amp-0.2.0~alpha-1-arm64.hpkg" \
    "$HAIKU_WORK/rock5-image-extras/packages/kiri-0.0.1~alpha-1-arm64.hpkg" \
    "$HAIKU_WORK/rock5-image-extras/packages/turbochook-0.1.2~alpha-1-arm64.hpkg"; do
    test -s "$needed" || { echo "Missing full-image input: $needed" >&2; exit 1; }
done

# Wi-Fi firmware comes from HaikuPorts' architecture-neutral packages
# (import-wifi-firmware-packages.sh); Bluetooth firmware packages are made by
# build-bluetooth-firmware-packages.sh. arm64 packagefs has no zstd support,
# so a zstd package would silently leave its drivers without firmware.
for needed in \
	"$HAIKU_WORK/rock5-image-extras/packages/intel_wifi_firmwares-2025_02_11-1-any.hpkg" \
	"$HAIKU_WORK/rock5-image-extras/packages/ralink_wifi_firmwares-2023_08_04-1-any.hpkg" \
	"$HAIKU_WORK/rock5-image-extras/packages/realtek_wifi_firmwares-2019_01_02-1-any.hpkg" \
	"$HAIKU_WORK/rock5-image-extras/packages/intel_bluetooth_firmwares-20240318-1-any.hpkg" \
	"$HAIKU_WORK/rock5-image-extras/packages/realtek_bluetooth_firmwares-20240318-1-any.hpkg" \
	"$HAIKU_WORK/rock5-image-extras/packages/mediatek_bluetooth_firmwares-20240318-1-any.hpkg"; do
	test -s "$needed" || { echo "Missing firmware package: $needed" >&2; exit 1; }
	[ "$(od -An -tu1 -j18 -N2 -- "$needed" | awk '{print $1 * 256 + $2}')" != 2 ] \
		|| { echo "zstd package, run import-wifi-firmware-packages.sh: $needed" >&2; exit 1; }
done

cd "$HAIKU_WORK/build/arm64"
log="$HAIKU_WORK/artifacts/rock5full-build-$(date -u +%Y%m%dT%H%M%SZ).log"
python3 "$HAIKU_SOURCE/tools/rock5-itx/build_info.py" start @rock5full-mmc
if ! jam -q -j"$HAIKU_JOBS" @rock5full-mmc > "$log" 2>&1; then
    tail -60 "$log" >&2
    exit 1
fi
python3 "$HAIKU_SOURCE/tools/rock5-itx/build_info.py" finish
printf 'Build: %s\nRecord: %s\n' "$log" "$HAIKU_WORK/build/arm64/full-build-record.json"
