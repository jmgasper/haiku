#!/bin/bash
# Work the OpenGL present path hard enough to shake out what a short run hides.
#
# Everything here has broken the machine at least once before: pinned pages
# released in the wrong address space, a stale frame buffer after a mode set,
# a window drawn into while it is being torn down. The point is to do all of
# it repeatedly and then check that the machine is still there, that every
# pinned range came back, and that the driver logged nothing new.
#
# usage: gl-soak.sh [seconds per phase]
set -u
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=10 ws-haiku"
T=${1:-45}

$SSH "set -u
	cd /boot/home/tests
	export VK_ICD_FILENAMES=/boot/home/build/install/data/vulkan/icd.d/nouveau_icd.x86_64.json
	export LD_LIBRARY_PATH=/boot/home/build/install/lib

	# The screen saver blanks the display, and a blanked desktop has no visible
	# windows, so nothing would be presented into the screen and the run would
	# prove nothing.
	kill \$(ps | grep '[s]creen_blanker' | awk '{print \$2}') 2>/dev/null

	reap() {
		ps | grep '[g]ltest' | awk '{print \$2}' | xargs -r kill -9 2>/dev/null
		sleep 2
	}
	reap

	before_locked=\$(grep -ac 'nvidia_rm: locked' /var/log/syslog)
	before_unlocked=\$(grep -ac 'os_unlock_user_pages' /var/log/syslog)
	before_lines=\$(wc -l < /var/log/syslog)

	echo '== three windows at once'
	GLTEST_POS=60,60    GLTEST_SIZE=800x600 ./gltest $T 2 >/tmp/s1.log 2>&1 &
	GLTEST_POS=900,80   GLTEST_SIZE=600x400 ./gltest $T 2 >/tmp/s2.log 2>&1 &
	GLTEST_POS=500,560  GLTEST_SIZE=700x450 ./gltest $T 2 >/tmp/s3.log 2>&1 &
	wait
	for f in /tmp/s1.log /tmp/s2.log /tmp/s3.log; do grep -ao '[0-9.]* fps.*' \$f; done
	reap

	echo '== opening and closing, over and over'
	for i in \$(seq 20); do
		GLTEST_POS=\$((100 + i * 20)),\$((100 + i * 10)) ./gltest 0.4 1 >/dev/null 2>&1
	done
	echo '  20 windows came and went'
	reap

	echo '== changing the mode underneath'
	# Long enough to outlast the mode changes, so that it reports rather than
	# being reaped in the middle of them.
	GLTEST_POS=200,200 GLTEST_SIZE=900x600 ./gltest 32 2 >/tmp/s4.log 2>&1 &
	gl=\$!
	sleep 4
	for i in \$(seq 4); do
		screenmode -q 1280 720 >/dev/null 2>&1; sleep 3
		screenmode -q 1920 1080 >/dev/null 2>&1; sleep 3
	done
	wait \$gl
	grep -ao '[0-9.]* fps.*' /tmp/s4.log || echo '  [!] it did not survive the mode changes'
	reap

	echo '== killed while the GPU is writing the screen'
	for i in \$(seq 5); do
		(GLTEST_POS=300,300 GLTEST_SIZE=800x600 ./gltest 30 2 >/dev/null 2>&1 &)
		sleep 3
		ps | grep '[g]ltest' | awk '{print \$2}' | xargs -r kill -9 2>/dev/null
		sleep 1
	done
	echo '  killed 5 times, still answering'
	reap

	echo '== a window that stops drawing, covered and uncovered'
	(GLTEST_POS=250,250 GLTEST_SIZE=900x600 GLTEST_IDLE=$T ./gltest 3 2 >/dev/null 2>&1 &)
	sleep 8
	for i in \$(seq 4); do
		/boot/system/apps/DeskCalc >/dev/null 2>&1 &
		sleep 3
		hey DeskCalc quit >/dev/null 2>&1
		sleep 2
	done
	echo '  covered and uncovered 4 times'
	reap

	echo '== afterwards'
	sleep 3
	locked=\$(grep -ac 'nvidia_rm: locked' /var/log/syslog)
	unlocked=\$(grep -ac 'os_unlock_user_pages' /var/log/syslog)
	echo \"  pinned ranges: \$((locked - before_locked)) locked, \$((unlocked - before_unlocked)) released\"
	echo '  anything new in the log the driver is unhappy about:'
	tail -n +\$before_lines /var/log/syslog | grep -ai 'nvidia\|NVRM\|nvkms' \
		| grep -vi 'locked\|unlock' | tail -10
	echo \"  \$(uptime)\"

	echo '== and it still draws'
	GLTEST_SIZE=800x600 ./gltest 3 2 2>&1 | grep -ao '[0-9.]* fps.*'
	reap"
