#!/bin/bash
# Check the workstation against what it is supposed to do, one line per thing.
#
# Every check here exists because something once looked fine and was not: a
# build that never installed, a driver the kernel never loaded, a screen saver
# that quietly turned the GPU path off, a Vulkan driver only visible to a shell
# with the right variables set. So nothing is run with anything set in its
# environment, and each check asks the machine rather than trusting that a
# thing was deployed.
#
# usage: check-workstation.sh
set -u
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=10 ws-haiku"

$SSH 'set -u
	pass=0; fail=0
	say() { # say <ok|no> <what> <detail>
		if [ "$1" = ok ]; then pass=$((pass+1)); printf "  yes  %-34s %s\n" "$2" "$3"
		else fail=$((fail+1)); printf "  NO   %-34s %s\n" "$2" "$3"; fi
	}

	echo "the machine"
	# sysinfo counts threads, and this part has sixteen cores with two each.
	threads=$(sysinfo | grep -c "^CPU #")
	[ "$threads" -ge 32 ] && say ok "sixteen cores" "$threads threads" \
		|| say no "sixteen cores" "$threads threads"

	# df prints a block of lines here rather than a table.
	boot=$(df /boot | grep "^ *Device:" | awk "{print \$2}")
	case "$boot" in
		*nvme*) say ok "booted from the NVMe drive" "$boot";;
		*) say no "booted from the NVMe drive" "${boot:-unknown}";;
	esac

	# There is a space after the colon, and the KVM presents a second
	# interface, so report every address that is not the loopback.
	ips=$(ifconfig | grep -o "inet addr: [0-9.]*" | awk "{print \$3}" \
		| grep -v "^127\." | tr "\n" " ")
	[ -n "$ips" ] && say ok "network" "$ips" || say no "network" "no address"

	echo "graphics"
	# Ask the device tree, not the syslog: the syslog is trimmed as it grows
	# and the driver announces itself only once, early.
	[ -e /dev/graphics/nvidia0 ] \
		&& say ok "the GPU driver started" "$(ls /dev/graphics | tr "\n" " ")" \
		|| say no "the GPU driver started" "no /dev/graphics/nvidia0"

	mode=$(screenmode -s 2>/dev/null)
	[ -n "$mode" ] && say ok "a display is being driven" "$mode" \
		|| say no "a display is being driven" "no mode"

	# Nothing set in the environment: this is how a program started from the
	# Deskbar sees the machine.
	vk=$(env -u VK_ICD_FILENAMES -u LD_LIBRARY_PATH /boot/home/tests/vkbench submit 2>&1 | grep -o "NVIDIA[^(]*" | head -1)
	[ -n "$vk" ] && say ok "Vulkan finds the GPU by itself" "$vk" \
		|| say no "Vulkan finds the GPU by itself" "falls back to software"

	blanker=$(ps | grep -c "[s]creen_blanker")
	[ "$blanker" -eq 0 ] && say ok "the screen saver is not hiding things" "" \
		|| say no "the screen saver is not hiding things" "kill it before measuring"

	rm -f /tmp/check-gl.log
	(cd /boot/home/tests && env -u VK_ICD_FILENAMES -u LD_LIBRARY_PATH \
		GLTEST_SIZE=800x600 ./gltest 2 2 >/tmp/check-gl.log 2>&1 &)
	sleep 12
	gl=$(grep -ao "renderer:.*" /tmp/check-gl.log | head -1)
	case "$gl" in
		*zink*) say ok "OpenGL runs on the GPU" "$(grep -ao "[0-9.]* fps.*" /tmp/check-gl.log)";;
		*) say no "OpenGL runs on the GPU" "${gl:-nothing rendered}";;
	esac
	grep -aq "present into the screen: 1" /tmp/check-gl.log 2>/dev/null
	ps | grep "[g]ltest" | awk "{print \$2}" | xargs -r kill -9 2>/dev/null

	r=$(cd /boot/home/tests && ./retracetest 2 2>/dev/null | tail -1)
	case "$r" in
		*"a second"*) say ok "the display reports its blanks" "$r";;
		*) say no "the display reports its blanks" "${r:-nothing}";;
	esac

	echo "sound"
	outputs=$(cd /boot/home/tests && ./audioout 2>/dev/null | grep -c "^[0-9]")
	[ "$outputs" -ge 2 ] && say ok "both outputs are there" "$outputs" \
		|| say no "both outputs are there" "$outputs"
	for i in 0 1; do
		(cd /boot/home/tests && ./audioout $i >/dev/null 2>&1)
		sleep 1
		rate=$(cd /boot/home/tests && ./soundtest 1.5 440 2>/dev/null | grep -o "[0-9]* frames a second" | tail -1)
		[ -n "$rate" ] && say ok "output $i clocks its stream" "$rate" \
			|| say no "output $i clocks its stream" "nothing came back"
	done
	(cd /boot/home/tests && ./audioout 0 >/dev/null 2>&1)

	echo "the rest"
	usb=$(listusb | grep -c RootHub)
	[ "$usb" -ge 5 ] && say ok "every USB controller is up" "$usb root hubs" \
		|| say no "every USB controller is up" "$usb root hubs"

	echo
	echo "$pass working, $fail not"
	[ "$fail" -eq 0 ]'
