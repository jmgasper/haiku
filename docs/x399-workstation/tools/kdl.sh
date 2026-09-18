#!/bin/bash
# Run kernel debugger commands on the workstation; output is read back from syslog.
# usage: kdl.sh "<cmd1>" ["<cmd2>" ...]
K=/mnt/HaikuWork/x399/tools/kvm
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=5 ws-haiku"
tag="kdl-$(date +%s)"
($SSH "kernel_debugger $tag" >/dev/null 2>&1 &)
sleep 4
for cmd in "$@"; do
	$K type "  $cmd\n" >/dev/null 2>&1
	sleep 2
done
$K type "  co\n" >/dev/null 2>&1
sleep 4
$SSH "grep -a -A400 'USER: $tag' /var/log/syslog | sed -n '1,/kdebug> *co/p'"
