#!/bin/bash
# Build the Low Energy bench tools on the workstation and run one of them.
#
# These talk to the radio over usb_raw rather than through the stack, so the
# Bluetooth server has to be out of the way: it holds the endpoints they read.
# The server is supervised, so it comes back by itself unless it is stopped
# through the launch roster rather than killed.
#
# A USB read waits for ever and only a kill interrupts it, hence the
# `timeout -s KILL` around every run.
#
# usage: deploy-btle.sh <btle|btlehid> [arguments for it]
set -uo pipefail
X399=/mnt/HaikuWork/x399
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
TESTS=$X399/haiku/docs/x399-workstation/tests
CRYPTO=$X399/haiku/src/libs/compat/openbsd_wlan/crypto

tool=${1:-btle}
shift || true

# The AES here is the tree's own, from the wireless compatibility layer. It is
# written for the kernel, so the two includes that only exist there are swapped
# for their userland equivalents; nothing else in it is kernel-specific.
sed -e 's|#include <sys/systm.h>|#include <string.h>|' \
	-e 's|#include <sys/stdint.h>|#include <stdint.h>|' \
	"$CRYPTO/aes.c" > /tmp/aes.c
cp "$CRYPTO/aes.h" /tmp/aes.h

scp -qF $X399/ssh/config /tmp/aes.c /tmp/aes.h "$TESTS/btle.cpp" \
	"$TESTS/btlehid.cpp" ws-haiku:/boot/home/tests/ || exit 1

# The headers the kernel was built with are kept beside the tests, because an
# ioctl number that does not match is not something either side can notice. The
# transport header reaches into the kernel's own utilities, which live with the
# other private headers.
#
# The old binaries go first: a build that fails while an executable from last
# time is still lying there runs the old one, which is worse than not running.
$SSH 'cd /boot/home/tests && rm -f btle btlehid
	gcc -O2 -c aes.c -o aes.o 2>&1 | head -5
	g++ -O2 -I include -I /system/develop/headers/private -o btle btle.cpp \
		2>&1 | head -20
	g++ -O2 -I include -I /system/develop/headers/private -o btlehid \
		btlehid.cpp aes.o 2>&1 | head -20
	test -x btle -a -x btlehid && echo "built"' | tee /tmp/btle-build.log
grep -q "^built$" /tmp/btle-build.log || { echo "the tools did not build" >&2; exit 1; }

# The server is supervised, so killing it only makes it come back; stopping it
# through the roster is what keeps it away. It does not exit instantly, and
# while it lives it owns the port, so wait for it to actually be gone.
$SSH 'launch_roster stop x-vnd.haiku-bluetooth_server >/dev/null 2>&1
	for i in $(seq 20); do
		ps | grep -q "[b]luetooth_server" || break
		sleep 1
	done
	ps | grep -q "[b]luetooth_server" \
		&& echo "the bluetooth server would not stop" || echo "server stopped"'

echo "--- running $tool $*"
timeout 300 $SSH "cd /boot/home/tests && timeout -s KILL 240 ./$tool $*"
status=$?
echo "--- $tool exited $status"
exit $status
