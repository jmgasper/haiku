#!/bin/bash
# Install the Bluetooth kernel side - the transport driver, the HCI module, the
# connection data module and L2CAP - and bring the machine back up on it.
#
# Kernel add-ons are looked for in the user's non-packaged directory before the
# ones the system ships, so that is where these go; a legacy driver is found
# through its dev/ tree rather than by its binary, hence the symlink.
#
# The modules share a structure whose shape all four have to agree on, so they
# are installed together even when only one of them changed. The radio also
# loses its firmware when the power goes, and the driver hands it that at open,
# so coming back up is part of installing rather than something separate.
#
# The test programs are built against the same headers the kernel was, because
# an ioctl number that does not match is not something either side can notice.
#
# usage: deploy-btstack.sh [--no-cycle]
set -uo pipefail
X399=/mnt/HaikuWork/x399
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
BUILT=$X399/build/x86_64/objects/haiku/x86_64/release/add-ons/kernel
HEADERS=$X399/haiku/headers
REMOTE=/boot/home/config/non-packaged/add-ons/kernel

cycle=yes
[ "${1:-}" = "--no-cycle" ] && cycle=no

cd $X399/build/x86_64
source $X399/tools/env.sh
jam -q hci btCoreData l2cap h2generic 2>&1 \
	| grep -E "^(.*error|.*Error)" && { echo "build failed" >&2; exit 1; }

for target in bluetooth/hci/hci bluetooth/btCoreData/btCoreData \
		network/protocols/bluetooth/l2cap/l2cap \
		drivers/bluetooth/h2/h2generic/h2generic; do
	[ -f "$BUILT/$target" ] || { echo "$target did not build" >&2; exit 1; }
done

$SSH "mkdir -p $REMOTE/bluetooth $REMOTE/network/protocols \
	$REMOTE/drivers/bin $REMOTE/drivers/dev/bluetooth/h2 \
	/boot/home/tests/include/bluetooth/HCI" || exit 1

scp -qF $X399/ssh/config "$BUILT/bluetooth/hci/hci" \
	"$BUILT/bluetooth/btCoreData/btCoreData" ws-haiku:$REMOTE/bluetooth/ || exit 1
scp -qF $X399/ssh/config "$BUILT/network/protocols/bluetooth/l2cap/l2cap" \
	ws-haiku:$REMOTE/network/protocols/ || exit 1
scp -qF $X399/ssh/config \
	"$BUILT/drivers/bluetooth/h2/h2generic/h2generic" \
	ws-haiku:$REMOTE/drivers/bin/h2generic || exit 1
scp -qF $X399/ssh/config \
	"$HEADERS/os/bluetooth/HCI/btHCI_transport.h" \
	"$HEADERS/os/bluetooth/HCI/btHCI_command.h" \
	"$HEADERS/os/bluetooth/HCI/btHCI_event.h" \
	"$HEADERS/os/bluetooth/HCI/btHCI.h" \
	"$HEADERS/os/bluetooth/HCI/btHCI_acl.h" \
	ws-haiku:/boot/home/tests/include/bluetooth/HCI/ || exit 1

$SSH "ln -sf ../../../bin/h2generic $REMOTE/drivers/dev/bluetooth/h2/h2generic
	sync" || exit 1
echo "installed the driver, the HCI and connection modules, and L2CAP"

if [ "$cycle" = no ]; then
	echo "not restarting: the running kernel still has the old ones"
	exit 0
fi

$X399/tools/power-cycle.sh
for i in $(seq 90); do $SSH 'uptime' >/dev/null 2>&1 && break; sleep 5; done
sleep 10

echo "--- what the driver found:"
$SSH 'grep -a "h2generic\|btCoreData\|mediatek" /var/log/syslog | tail -8'
echo "--- the stack can reach the adapter:"
$SSH 'cd /boot/home/tests && timeout 60 ./bttest 1 2>/dev/null \
	| grep -E "^adapter|^  name|no local"'
