#!/bin/bash
# Deploy nvidia_rm to the running workstation and open the GPU device.
set -u
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=5 ws-haiku"
OUT=/mnt/HaikuWork/x399/build/nvidia_rm/out
LOG=/mnt/HaikuWork/x399/evidence/nvtest-$(date +%H%M%S)
tar -C $OUT -cf /mnt/HaikuWork/tmp/nvidia_rm.tar add-ons
$SSH 'cd /boot/system/non-packaged && tar -xf - && sync' < /mnt/HaikuWork/tmp/nvidia_rm.tar || exit 1
sleep 5
timeout 90 $SSH 'syslog_start=$(wc -l < /var/log/syslog); exec 3</dev/graphics/nvidia0 && echo OPENED; sleep 3; tail -n +$syslog_start /var/log/syslog | grep -v "DHCP\|sshd" | tail -80' > $LOG.txt 2>&1
echo "exit=$?" >> $LOG.txt
/mnt/HaikuWork/x399/tools/kvm screenshot $LOG.jpg >/dev/null 2>&1
cat $LOG.txt | tail -60
echo "screenshot: $LOG.jpg"
