# Does the Pi take reports from the NanoKVM's keyboard, mouse and tablet?
# Runs on the KVM:  ssh -F /mnt/HaikuWork/nanokvm/.ssh/config nanokvm 'sh -s' < kvm-hid-probe.sh
# A write to /dev/hidgN blocks while the previous report still waits for the
# host, so 20 of 20 means the host keeps polling that endpoint. The reports
# press nothing (the pointer ends near the left edge). The KVM's busybox has
# no `timeout`.
try() { # device, report
  printf "$2" > /dev/hidg$1 &
  pid=$!
  n=0
  while kill -0 $pid 2>/dev/null && [ $n -lt 10 ]; do usleep 50000; n=$((n+1)); done
  if kill -0 $pid 2>/dev/null; then kill $pid 2>/dev/null; wait $pid 2>/dev/null; return 1; fi
  wait $pid
}
k=0; m=0; t=0
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  try 0 '\0\0\0\0\0\0\0\0' && k=$((k+1))
  if [ $((i % 2)) = 0 ]; then try 1 '\0\1\1\0' && m=$((m+1)); else try 1 '\0\377\377\0' && m=$((m+1)); fi
  try 2 '\0\0\020\0\100\0' && t=$((t+1))
done
echo "delivered of 20: keyboard=$k mouse=$m tablet=$t udc=$(cat /sys/class/udc/*/state)"
