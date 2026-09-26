#!/usr/bin/env bash
# Build Haiku's WPA supplicant for the ARM64 image without an OpenSSL package.
set -euo pipefail
source "$(dirname "$0")/env.sh"

revision=bdf3144ad606b226aca6f8fae103e671e4655138
archive_sha256=e7e36b4ed5d6ff0e6a233b427b312c6984ab01d79e219d0d4b9c9e2ccf86f8ab
archive="$HAIKU_WORK/cache/wpa_supplicant-$revision.tar.gz"
build="$HAIKU_WORK/build/wpa_supplicant-$revision"
sdk="$HAIKU_WORK/artifacts/mesa-reconstruction/20260915T131119Z-96b4bc/sysroot"
cross="$HAIKU_WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-"
host_tools="$HAIKU_WORK/build/arm64/objects/linux/x86_64/release/tools"
output="$HAIKU_WORK/rock5-image-extras/packages/wpa_supplicant-2.11.haiku.1-1-arm64.hpkg"

test -d "$sdk/boot/system/develop/headers/private/libs/compat/freebsd_wlan"
test -x "$host_tools/package/package"
if [[ ! -f "$archive" ]]; then
	curl -fL "https://github.com/haiku/wpa_supplicant/archive/$revision.tar.gz" \
		-o "$archive"
fi
printf '%s  %s\n' "$archive_sha256" "$archive" | sha256sum -c -
if [[ ! -d "$build" ]]; then
	tar -xf "$archive" -C "$HAIKU_WORK/build"
fi

config="$build/wpa_supplicant/.config"
if ! grep -q '^CONFIG_INTERNAL_LIBTOMMATH=y$' "$config"; then
	sed -i "s|-I/system/develop/|-I$sdk/boot/system/develop/|g" "$config"
	printf '\nCONFIG_TLS=internal\nCONFIG_INTERNAL_LIBTOMMATH=y\n' >> "$config"
	# Haiku's mimeset cannot set attributes on a Linux host. resattr sets
	# equivalent package attributes on the staged executable below.
	sed -i 's/^[[:space:]]*mimeset -F wpa_supplicant$/\ttrue/' \
		"$build/wpa_supplicant/Makefile"
fi

export PATH="$host_tools/rc:$host_tools:$PATH"
make -C "$build/wpa_supplicant" -j"$HAIKU_JOBS" wpa_supplicant \
	CC="${cross}gcc --sysroot=$sdk" LDO="${cross}g++ --sysroot=$sdk"

stage=$(mktemp -d "$HAIKU_WORK/tmp/wpa-package-XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
mkdir -p "$stage/bin"
install -m 0755 "$build/wpa_supplicant/wpa_supplicant" "$stage/bin/wpa_supplicant"
"${cross}strip" --strip-debug "$stage/bin/wpa_supplicant"
"$host_tools/xres" -o "$stage/bin/wpa_supplicant" \
	"$build/wpa_supplicant/wpa_gui-haiku/wpa_supplicant.rsrc"
"$host_tools/resattr/resattr" -O -o "$stage/bin/wpa_supplicant" \
	"$build/wpa_supplicant/wpa_gui-haiku/wpa_supplicant.rsrc"
cat > "$stage/.PackageInfo" <<'EOF'
name wpa_supplicant
version 2.11.haiku.1-1
architecture arm64
summary "WPA/WPA2 wireless supplicant for Haiku"
description "Wireless authentication and association for the network server."
packager "ROCK 5 ITX fork"
vendor "ROCK 5 ITX fork"
urls { "https://github.com/haiku/wpa_supplicant" }
copyrights { "2003-2026 Jouni Malinen and contributors" }
licenses { "BSD (2-clause)" }
provides {
	wpa_supplicant = 2.11.haiku.1
	cmd:wpa_supplicant = 2.11.haiku.1
}
requires {
	haiku >= r1~beta6
}
EOF
"$host_tools/package/package" create -C "$stage" "$output"
"$host_tools/package/package" list -a "$output" | grep -q 'BEOS:APP_SIG'
"${cross}readelf" -h "$stage/bin/wpa_supplicant" | grep -q 'AArch64'
sha256sum "$output"
