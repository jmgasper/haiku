#!/usr/bin/env bash
# Fetch the Radxa Cubie A7S boot loader: Allwinner's boot0 (DRAM setup),
# ARM Trusted Firmware, the SCP firmware and U-Boot 2026.04 in one file,
# from Radxa's u-boot-dlan17 package. It is written at 128 KiB on the card.
#   fetch-bootloader.sh [directory]   (default /mnt/HaikuWork/cubie/bootloader)
# Prints the path of u-boot-sunxi-with-spl.bin.
set -euo pipefail
VERSION=2026.04-4
SHA256=4eb6eaf7be5b27472c9475e0a9eb10960b71d741704344b4ba925938bbe2c19f
DIR=${1:-/mnt/HaikuWork/cubie/bootloader}/$VERSION
BIN=$DIR/u-boot-sunxi-with-spl.bin

if [[ -f $BIN ]] && echo "$SHA256  $BIN" | sha256sum -c --quiet 2>/dev/null; then
	echo "$BIN"
	exit 0
fi
mkdir -p "$DIR"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/cubie-bootloader.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
curl -sSfL -o "$WORK/u-boot.deb" \
	"https://github.com/radxa-pkg/u-boot-dlan17/releases/download/$VERSION/u-boot-dlan17_${VERSION}_all.deb"
(cd "$WORK" && ar x u-boot.deb && tar xf data.tar.*)
cp "$WORK/usr/lib/u-boot/radxa-cubie-a7s/u-boot-sunxi-with-spl.bin" "$BIN.new"
echo "$SHA256  $BIN.new" | sha256sum -c --quiet
mv "$BIN.new" "$BIN"
echo "$BIN"
