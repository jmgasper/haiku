#!/bin/sh
# sleeptest.sh <seconds asleep> <tag>: DPMS off, wait, DPMS on, and read the
# DisplayPort link state of each monitor right after waking.
d=/boot/home/x399-tests
secs=$1; tag=$2; out=$d/sleep-$tag.txt
mark=$(wc -l < /var/log/syslog)
{
echo "== before $(date +%T)"; $d/nvdpyinfo --dpcd
$d/dpmstest off; echo "== off $(date +%T)"
sleep $secs
echo "== waking $(date +%T)"; $d/dpmstest on
for t in 0 1 3 10; do sleep $t; echo "== +$t s"; $d/nvdpyinfo --dpcd; done
$d/dpmstest state
echo "== syslog"
tail -n +$mark /var/log/syslog | grep -E "hot plug|display change|nvidia_rm|app_server|NvAccelerant::(Set|Apply)" | grep -v -E "Dpms(Capabilities|Mode$)"
echo "== layout"; screenmode -d | grep -E "^[0-9]|scale"
echo DONE
} > $out 2>&1
