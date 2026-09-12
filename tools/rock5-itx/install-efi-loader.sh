#!/bin/sh
# Run inside Haiku on an already identified and mounted lab EFI partition.
# Keep the current loader intact until both replacement and backup verify.
set -eu
[ "$(uname -s)" = Haiku ] || { echo 'Run this helper inside Haiku.' >&2; exit 2; }
[ "$#" = 3 ] || { echo "Usage: $0 EFI_BOOT_DIRECTORY OLD_SHA256 NEW_SHA256" >&2; exit 2; }
loader_directory=$1
expected_old=$2
expected_new=$3
for checksum in "$expected_old" "$expected_new"; do
    [ "${#checksum}" = 64 ] || exit 2
    case "$checksum" in *[!0-9a-f]*) exit 2;; esac
done
[ -d "$loader_directory" ]
source_loader=/boot/system/data/platform_loaders/haiku_loader.efi
loader="$loader_directory/BOOTAA64.EFI"
backup="$loader_directory/BOOTAA64.EFI.rock5-previous"
staged="$loader_directory/BOOTAA64.EFI.rock5-staged"
file_hash() {
    hash_result=$(sha256sum "$1") || return
    printf '%s\n' "${hash_result%% *}"
}
[ "$(file_hash "$source_loader")" = "$expected_new" ]
current_hash=$(file_hash "$loader")
if [ "$current_hash" = "$expected_new" ]; then
    echo "ROCK5_EFI_LOADER_ALREADY_CURRENT $expected_new"
    exit 0
fi
[ "$current_hash" = "$expected_old" ]
# An interrupted attempt may have left a verified backup; never overwrite it.
if [ ! -e "$backup" ]; then
    cp "$loader" "$backup"
fi
[ "$(file_hash "$backup")" = "$expected_old" ]
# A stale staging file must be inspected rather than opened with O_TRUNC.
[ ! -e "$staged" ]
cp "$source_loader" "$staged"
[ "$(file_hash "$staged")" = "$expected_new" ]
sync
# FAT's read-only attribute can reject writes after O_TRUNC has emptied a
# destination. Only rename the verified new file over the old path.
chmod u+w "$loader" "$staged"
mv -f "$staged" "$loader"
sync
[ "$(file_hash "$loader")" = "$expected_new" ]
[ "$(file_hash "$backup")" = "$expected_old" ]
echo "ROCK5_EFI_LOADER_INSTALLED $expected_new"
echo "ROCK5_EFI_LOADER_BACKUP $expected_old"
