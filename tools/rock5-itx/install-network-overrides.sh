#!/bin/sh
# Run inside the dedicated Haiku lab installation after component qualification.
set -eu
umask 077
[ "$#" -eq 4 ] || { echo "usage: $0 RNDIS_FILE RNDIS_SHA STACK_FILE STACK_SHA" >&2; exit 2; }
rndis_source=$1
rndis_sha=$2
stack_source=$3
stack_sha=$4
root=/boot/home/config/non-packaged/add-ons/kernel
rndis=$root/drivers/bin/usb_rndis
device=$root/drivers/dev/net/usb_rndis
stack=$root/network/stack

check_hash()
{
    actual=$(sha256sum "$1")
    [ "${actual%% *}" = "$2" ]
}

for source in "$rndis_source" "$stack_source"; do
    case "$source" in /boot/home/rock5-lab/*) ;; *) exit 2 ;; esac
done
check_hash "$rndis_source" "$rndis_sha"
check_hash "$stack_source" "$stack_sha"

if [ -e "$rndis" ] || [ -L "$rndis" ] || [ -e "$stack" ] || [ -L "$stack" ] \
    || [ -e "$device" ] || [ -L "$device" ]; then
    check_hash "$rndis" "$rndis_sha"
    check_hash "$stack" "$stack_sha"
    [ -L "$device" ] && [ "$(readlink "$device")" = ../../bin/usb_rndis ]
    echo ROCK5_NETWORK_OVERRIDES_ALREADY_INSTALLED
    exit 0
fi

rndis_stage=/boot/home/rock5-lab/.usb-rndis-stage-$$
stack_stage=/boot/home/rock5-lab/.network-stack-stage-$$
[ ! -e "$rndis_stage" ] && [ ! -e "$stack_stage" ]
trap 'rm -f "$rndis_stage" "$stack_stage"' EXIT
cp "$rndis_source" "$rndis_stage"
cp "$stack_source" "$stack_stage"
chmod 755 "$rndis_stage" "$stack_stage"
check_hash "$rndis_stage" "$rndis_sha"
check_hash "$stack_stage" "$stack_sha"
sync
mkdir -p "$root/drivers/bin" "$root/drivers/dev/net" "$root/network"
mv "$stack_stage" "$stack"
mv "$rndis_stage" "$rndis"
ln -s ../../bin/usb_rndis "$device"
sync
check_hash "$rndis" "$rndis_sha"
check_hash "$stack" "$stack_sha"
[ "$(readlink "$device")" = ../../bin/usb_rndis ]
echo ROCK5_NETWORK_OVERRIDES_INSTALLED_REBOOT_REQUIRED
