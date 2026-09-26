#!/usr/bin/env bash
# Stage the AX210 Bluetooth firmware from a local linux-firmware installation.
# Binary firmware and its license stay outside the source repository.
set -euo pipefail
source "$(dirname "$0")/env.sh"

if [[ $# -ne 3 ]]; then
	echo "usage: $0 ibt-0041-0041.sfi.zst ibt-0040-0041.ddc.zst LICENCE.ibt_firmware.gz" >&2
	exit 2
fi

stage=$(mktemp -d "$HAIKU_WORK/tmp/ax210-bluetooth-XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
zstd -dc -- "$1" > "$stage/ibt-0041-0041.sfi"
zstd -dc -- "$2" > "$stage/ibt-0041-0041.ddc"
gzip -dc -- "$3" > "$stage/Intel Bluetooth Firmware License"

printf '%s  %s\n' \
	b3b5d5e48ca08187ce5d3553fb6daadb4c5f3e24972b5813fe91b60dbc888038 \
	"$stage/ibt-0041-0041.sfi" | sha256sum -c -
printf '%s  %s\n' \
	fe272982577efdc289cfe3e8bedafca607c0b7d6c9df1aab1880e22ef930d077 \
	"$stage/ibt-0041-0041.ddc" | sha256sum -c -
printf '%s  %s\n' \
	5181b0b51efc79d5acb2c9bb92042878fdbad97a92114d4ab5e32e2b5b52fce4 \
	"$stage/Intel Bluetooth Firmware License" | sha256sum -c -

destination="$HAIKU_WORK/rock5-image-extras/firmware/intel"
licenses="$HAIKU_WORK/rock5-image-extras/licenses"
mkdir -p "$destination" "$licenses"
install -m 0444 "$stage/ibt-0041-0041.sfi" "$destination/"
install -m 0444 "$stage/ibt-0041-0041.ddc" "$destination/"
install -m 0444 "$stage/Intel Bluetooth Firmware License" "$licenses/"
