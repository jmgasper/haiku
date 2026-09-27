#!/bin/sh
# hpdlog.sh <seconds>: timestamp every hot plug / layout line the accelerant
# writes to the syslog, for that long.
end=$(( $(date +%s) + $1 ))
last=$(wc -l < /var/log/syslog)
while [ $(date +%s) -lt $end ]; do
	now=$(wc -l < /var/log/syslog)
	if [ "$now" -lt "$last" ]; then last=0; fi
	if [ "$now" -gt "$last" ]; then
		tail -n +$((last + 1)) /var/log/syslog | head -n $((now - last)) \
			| grep -E "hot plug|display change|nvidia_rm: (DP|layout)|SetDpmsMode" \
			| sed "s/^/$(date +%T) /"
		last=$now
	fi
	sleep 1
done
