#!/usr/bin/env bash
# Fetch the firmware of the Raspberry Pi 4's Wi-Fi chip (CYW43455) from
# Raspberry Pi's firmware-nonfree repository, under the names the
# broadcomfmac driver asks for. Redistributable (Cypress licence), not part
# of this tree.
#   fetch-wifi-firmware.sh [directory]
set -euo pipefail
DIR=${1:-/mnt/HaikuWork/rpi4/wifi/firmware}
BASE=https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/bookworm/debian/config/brcm80211
mkdir -p "$DIR"
fetch() {
    [ -s "$DIR/$1" ] || curl -sfL --retry 3 -o "$DIR/$1" "$BASE/$2"
}
fetch brcmfmac43455-sdio.bin cypress/cyfmac43455-sdio-standard.bin
fetch brcmfmac43455-sdio.clm_blob cypress/cyfmac43455-sdio.clm_blob
fetch brcmfmac43455-sdio.txt brcm/brcmfmac43455-sdio.txt
(cd "$DIR" && sha256sum brcmfmac43455-sdio.* > SHA256SUMS)
echo "CYW43455 firmware -> $DIR"
