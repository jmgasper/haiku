#!/bin/sh
# rd-hang-test.sh ROUNDS [BUNDLE] : runs Summit from a RAM disk, force-kills
# the browser, removes the bundle at once (its helpers still run from it),
# and checks that the RAM disk still answers.
n=${1:-5}; name=${2:-b-final6}; rd=/boot/home/rdtest
mkdir -p $rd
if ! df $rd 2>/dev/null | grep -q ramfs; then mount -t ramfs $rd || exit 1; fi
i=1
while [ $i -le $n ]; do
  rm -rf $rd/$name
  cp -a /Documents/SummitPi/$name $rd/$name || exit 1
  chmod 755 $rd/$name/Summit $rd/$name/WebProcess $rd/$name/NetworkProcess
  ln -sf libWebKit.so.1 $rd/$name/lib/libWebKit.so
  if [ -n "$PRESSURE" ]; then rpi4_swap_commit 32 | tail -1; fi
  $rd/$name/Summit --profile /boot/home/rdtest-profile https://example.com/ > /boot/home/rdtest.log 2>&1 &
  pid=$!
  sleep 9
  kill -9 $pid
  rm -rf $rd/$name &
  sleep 6
  ls -la $rd > /boot/home/rdtest.ls 2>&1 &
  lspid=$!
  sleep 4
  if kill -0 $lspid 2>/dev/null; then echo "round $i: ls HANGS"; exit 2; fi
  left=$(ps | grep -E "rdtest/" | grep -v grep | awk '{print $(NF-3)}' | tr '\n' ' ')
  for t in $left; do kill -9 $t 2>/dev/null; done
  sleep 2
  stuck=$(ps | grep -E "rdtest/" | grep -v grep | awk '{print $(NF-3) ":" $1}' | tr '\n' ' ')
  echo "round $i: helpers left [$left] stuck after kill [$stuck] $(sysinfo -mem | head -1)"
  i=$((i + 1))
done
echo done
