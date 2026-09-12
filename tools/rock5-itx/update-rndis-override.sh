#!/bin/sh
# Run inside the dedicated Haiku installation after the candidate's QEMU gate.
set -eu

[ "$#" = 4 ] || { echo "usage: $0 SOURCE NEW_SHA OLD_SHA STACK_SHA" >&2; exit 2; }
source=$1
new_sha=$2
old_sha=$3
stack_sha=$4
for value in "$new_sha" "$old_sha" "$stack_sha"; do
	[ "${#value}" = 64 ] || exit 2
	case "$value" in *[!0-9a-f]*) exit 2;; esac
done

lab=/boot/home/rock5-lab
case "$source" in "$lab"/*) ;; *) exit 2;; esac
case "$source" in */../*|*/./*) exit 2;; esac
drivers=/boot/home/config/non-packaged/add-ons/kernel/drivers
driver=$drivers/bin/usb_rndis
stack=/boot/home/config/non-packaged/add-ons/kernel/network/stack
backup=$lab/usb_rndis-$old_sha.previous

check_hash()
{
	[ -f "$1" ] && [ ! -L "$1" ] || return 1
	actual=$(sha256sum "$1")
	[ "${actual%% *}" = "$2" ]
}

check_hash "$source" "$new_sha"
check_hash "$stack" "$stack_sha"
[ "$(readlink "$drivers/dev/net/usb_rndis")" = ../../bin/usb_rndis ]

if check_hash "$driver" "$new_sha"; then
	check_hash "$backup" "$old_sha"
	echo ROCK5_RNDIS_UPDATE_ALREADY_INSTALLED
	exit 0
fi
if [ -e "$driver" ] || [ -L "$driver" ]; then
	check_hash "$driver" "$old_sha"
	# Keep the loaded inode linked. Copying it and overwriting its old name
	# leaves an open, unlinked BFS inode whose blocks survive a reboot.
	[ ! -e "$backup" ] && [ ! -L "$backup" ]
else
	# Resume an interrupted update after the original was renamed.
	check_hash "$backup" "$old_sha"
fi

stage=$lab/.usb-rndis-update-$$
[ ! -e "$stage" ] && [ ! -L "$stage" ]
trap 'rm -f "$stage"' EXIT HUP INT TERM
cp "$source" "$stage"
chmod 755 "$stage"
check_hash "$stage" "$new_sha"
sync
if [ -e "$driver" ]; then
	mv "$driver" "$backup"
	check_hash "$backup" "$old_sha"
	sync
fi
mv -f "$stage" "$driver"
sync
check_hash "$driver" "$new_sha"
check_hash "$backup" "$old_sha"
echo ROCK5_RNDIS_UPDATE_INSTALLED_REBOOT_REQUIRED
