#!/bin/sh
# Open a Terminal on the live Haiku desktop via the KVM and type a command.
K=/mnt/HaikuWork/x399/tools/kvm
$K click 1225 721 >/dev/null 2>&1   # dismiss first boot prompt if shown
sleep 6
$K click 1850 14 >/dev/null; sleep 1
$K click 1681 226 >/dev/null; sleep 3
$K type 'Terminal' >/dev/null; sleep 1
$K key ALT O >/dev/null; sleep 4
$K type "$1\n" >/dev/null
