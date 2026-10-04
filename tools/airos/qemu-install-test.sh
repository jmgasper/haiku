#!/usr/bin/env bash
# Install the air/OS ISO onto a whole disk in arm64 QEMU through the
# Installer's UI, the way a user does (clicks at the positions of a 1024x768
# screen), then leave QEMU running for a look at the result.
#   qemu-install-test.sh DIR ISO DISK_INDEX
# DISK_INDEX 0 is the first disk under "Erase a whole disk" (a blank 8 GiB
# NVMe), 1 a 6 GiB NVMe with an MBR and three partitions, one of them FAT32.
# CDROM=1 attaches the ISO as a USB CD-ROM instead of a read-only USB disk.
# Screenshots land in DIR; the second disk is DIR/used.img.
#   python3 tools/airos/qemu_vm.py start DIR --disk blank:DIR/blank.img
# then boots the installed disk alone.
set -euo pipefail
cd "$(dirname "$0")"
DIR=$(realpath -m "$1") ISO=$2 INDEX=$3
v() { python3 qemu_vm.py "$@"; }
wait_for() { timeout "$2" bash -c "until grep -q -a -E '$1' $DIR/serial.log 2>/dev/null; do sleep 3; done"; }
pause() { timeout "$1" tail -f /dev/null || true; }

python3 vm.py stop "$DIR" >/dev/null 2>&1 || true
pause 2
rm -rf "$DIR"
mkdir -p "$DIR"
truncate -s 6G "$DIR/used.img"
printf 'label: dos\n,1G,c\n,2G,83\n,,7\n' | sfdisk -q "$DIR/used.img"
mkfs.fat -F 32 -n OLDDATA --offset 2048 "$DIR/used.img" $((1024 * 1024)) >/dev/null
v start "$DIR" ${CDROM:+--cdrom} --iso "$ISO" --disk blank:8G --disk "used:$DIR/used.img" >/dev/null
wait_for 'BootPrompt|FirstBoot' 400
pause 20
v shot "$DIR" 01-prompt >/dev/null
v click "$DIR" 648 566 >/dev/null; pause 12          # Install air/OS
v shot "$DIR" 02-notice >/dev/null
v click "$DIR" 682 501 >/dev/null; pause 12          # Continue
v shot "$DIR" 03-installer >/dev/null
v click "$DIR" 427 413 >/dev/null; pause 3           # Onto:
v shot "$DIR" 04-menu >/dev/null
v click "$DIR" 420 $((432 + 20 * INDEX)) >/dev/null; pause 3
v click "$DIR" 762 500 >/dev/null; pause 5           # Begin
v shot "$DIR" 05-alert >/dev/null
# the "Erase disk and install" button: between its 144 top and 114 bottom borders
y=$(python3 - "$DIR/05-alert.png" <<'EOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
top = None
for y in range(380, 620):
	v = im.getpixel((463, y))[0]
	if v == 144 and top is None:
		top = y
	elif v == 114 and top is not None and y - top > 15:
		print((top + y) // 2); break
EOF
)
echo "erase button at y=$y"
v click "$DIR" 463 "$y" >/dev/null
# The progress bar (white) shows while installing and goes away at the end.
seen=0
for i in $(seq 1 60); do
	pause 10
	v shot "$DIR" 06-progress >/dev/null
	if grep -a -q 'Killing team' "$DIR/serial.log"; then echo CRASH; break; fi
	bar=$(python3 -c "from PIL import Image; print(Image.open('$DIR/06-progress.png').convert('RGB').getpixel((500, 486))[0])")
	if [ "$bar" -ge 240 ]; then seen=1; elif [ $seen = 1 ]; then break; fi
done
pause 3
v shot "$DIR" 07-done
