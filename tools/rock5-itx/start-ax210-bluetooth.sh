#!/bin/sh
# Bring up Bluetooth at login: bt_firmware loads firmware into Intel
# controllers (every Intel combo card from the 7260 to the BE200 starts in ROM
# or bootloader mode) and Realtek ones (RTL8723 to RTL8922), then
# bluetooth_server starts for whatever adapter h2generic found. MediaTek
# MT7921/MT7922 radios get their firmware from h2generic itself.
# The file keeps its AX210-era name because UserBootscript and the
# Bluetooth preferences' start-services hook call it by that name.

server=/boot/system/servers/bluetooth_server
override=/boot/home/config/non-packaged/servers/bluetooth_server
[ -x "$override" ] && server=$override

loader=
for candidate in /boot/home/config/non-packaged/bin/bt_firmware \
		/boot/system/non-packaged/bin/bt_firmware \
		/boot/system/bin/bt_firmware; do
	if [ -x "$candidate" ]; then
		loader=$candidate
		break
	fi
done

# USB enumeration of the adapter can trail the desktop by several seconds.
for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	[ -e /dev/bluetooth/h2/h2generic/0 ] && break
	sleep 1
done
[ -e /dev/bluetooth/h2/h2generic/0 ] || exit 0

if [ -n "$loader" ] && "$loader" --present >/dev/null 2>&1; then
	# The loader needs the USB endpoints to itself.
	"$server" --finish >/dev/null 2>&1 || true
	# A controller that failed stays unusable, but other adapters still
	# need the server.
	"$loader" || echo 'Bluetooth firmware initialization failed' >&2
fi

"$server" >/dev/null 2>&1 &
