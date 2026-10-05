#!/usr/bin/env bash
# Enter the kernel debugger of a running (or wedged) board over serial.
KVM=(ssh -F /mnt/HaikuWork/nanokvm/.ssh/config -o ConnectTimeout=10 nanokvm)
"${KVM[@]}" 'stty -F /dev/ttyS1 115200 raw -echo -crtscts; for i in 1 2 3; do printf "+" > /dev/ttyS1; sleep 1; done'
