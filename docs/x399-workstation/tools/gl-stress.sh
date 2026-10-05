#!/bin/bash
# Exercise the OpenGL path the way that wedged the machine: run the test
# repeatedly, both letting it finish and killing it while the GPU is writing
# into the window's bitmap, and check that every pinned range is released.
#
# usage: gl-stress.sh [clean runs] [killed runs]
set -u
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=10 ws-haiku"
CLEAN=${1:-5}
KILLED=${2:-3}

$SSH "set -u
	cd /boot/home/tests
	export VK_ICD_FILENAMES=/boot/home/build/install/data/vulkan/icd.d/nouveau_icd.x86_64.json
	export LD_LIBRARY_PATH=/boot/home/build/install/lib
	before=\$(grep -ac 'os_lock_user_pages' /var/log/syslog)

	for i in \$(seq $CLEAN); do
		./gltest 2 2 2>&1 | grep -o '[0-9.]* fps.*'
	done

	for i in \$(seq $KILLED); do
		(./gltest 10 2 >/dev/null 2>&1 &)
		sleep 3
		kill -9 \$(ps | grep '[g]ltest' | awk '{print \$2}') 2>/dev/null
		sleep 2
		echo \"killed run \$i: machine still answering\"
	done

	sleep 2
	locked=\$(grep -ac 'nvidia_rm: locked' /var/log/syslog)
	unlocked=\$(grep -ac 'os_unlock_user_pages' /var/log/syslog)
	echo \"pinned ranges: \$locked locked, \$unlocked released\"
	grep -a 'os_unlock_user_pages' /var/log/syslog | tail -3
	uptime"
