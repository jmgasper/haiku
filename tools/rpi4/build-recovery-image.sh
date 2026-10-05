#!/bin/bash
# Build the Raspberry Pi 4 lab "recovery OS" image: Alpine Linux (diskless,
# runs from RAM) on one FAT32 partition, booted by the Pi's EEPROM from the
# NanoKVM's virtual USB disk. It brings up eth0 with DHCP and sshd with
# root login by public key, so this host can rewrite the SD card over ssh.
#
# Usage:
#   build-recovery-image.sh            build $WORK/rpi4-recovery.img
#   build-recovery-image.sh deploy     build, upload to the NanoKVM, attach
#                                      as a writable USB disk (no power cycle)
#   build-recovery-image.sh attach     attach the uploaded image only
#   build-recovery-image.sh detach     detach it (the EEPROM then boots the SD)
# Power the Pi off before deploy/attach/detach, and cycle it afterwards
# (power.py off; ...; power.py on): the running recovery OS keeps the disk
# mounted.
#
# Needs no root: sfdisk, mkfs.vfat --offset and mtools write the image file
# directly. Everything (downloads, temp files, output) stays under $WORK.
# No credentials live here. The sshd host key is generated once into
# $STATE/recovery-hostkeys so the Pi keeps one identity across rebuilds.

set -euo pipefail

WORK=${RPI4_RECOVERY_WORK:-/mnt/HaikuWork/rpi4/recovery}
STATE=${RPI4_STATE:-/mnt/HaikuWork/rpi4/state}
PUBKEY=${RPI4_RECOVERY_PUBKEY:-/mnt/HaikuWork/nanokvm/.ssh/id_ed25519.pub}
KVM_SSH_CONFIG=${NANOKVM_SSH_CONFIG:-/mnt/HaikuWork/nanokvm/.ssh/config}
KVM_HOST=${NANOKVM_SSH_HOST:-nanokvm}
KVM_IMAGE=${RPI4_RECOVERY_KVM_IMAGE:-/data/rpi4-recovery.img}
IMAGE=${RPI4_RECOVERY_IMAGE:-$WORK/rpi4-recovery.img}
IMAGE_MIB=${RPI4_RECOVERY_IMAGE_MIB:-384}
HOSTNAME_PI=rpi4-recovery

ALPINE_BRANCH=v3.24
ALPINE_VERSION=3.24.2
ALPINE_SHA256=60e6b9f49ec3d2c9660f2ef775f5af3a98cfd2f939c8f55b6f215d00e5ffcb87
ALPINE_MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
APK_STATIC_VERSION=3.0.8-r0
APK_STATIC_KEY=alpine-devel@lists.alpinelinux.org-6165ee59.rsa.pub

# raspberrypi/rpi-eeprom, pinned. The Pi 4 images listed here are copied to
# the image so an EEPROM config change needs no network.
RPI_EEPROM_REF=${RPI_EEPROM_REF:-a72213d02af27f2dc3b86f584988aec407e46378}
RPI_EEPROM_FILES=${RPI_EEPROM_FILES:-"rpi-eeprom-config rpi-eeprom-update rpi-eeprom-digest rpi-eeprom-update-default firmware-2711/old/stable/pieeprom-2022-01-25.bin firmware-2711/default/pieeprom-2026-09-23.bin firmware-2711/default/recovery.bin firmware-2711/default/vl805-000138c0.bin"}
# Extra EEPROM images (paths inside the rpi-eeprom repository), for example
# the release the board already runs.
RPI_EEPROM_EXTRA_FILES=${RPI_EEPROM_EXTRA_FILES:-}

# Installed from the image's own package repository by Alpine's init.
BASE_PACKAGES="alpine-base chrony openssh sfdisk partx e2fsprogs"
# Fetched here with their dependencies and installed offline at boot.
EXTRA_PACKAGES="python3 bash raspberrypi-utils-vcgencmd raspberrypi-utils-vcmailbox raspberrypi-utils-pinctrl dtc usbutils pciutils dosfstools mtools curl lsblk blkid parted xz zstd pv picocom rsync flashrom"

export TMPDIR=$WORK/tmp
mkdir -p "$WORK/dl" "$TMPDIR"

for dir in /mnt/HaikuWork/toolchains/host/usr/bin; do
	[ -x "$dir/mcopy" ] && PATH=$PATH:$dir
done
PATH=$PATH:/usr/sbin:/sbin

kvm_ssh() {
	ssh -F "$KVM_SSH_CONFIG" "$KVM_HOST" "$@"
}

# The NanoKVM re-applies /boot/usb.disk0 whenever its gadget rebinds, so
# keep it naming the same file as the live LUN. The LUN is writable (ro=0):
# an EEPROM self-update needs pieeprom.upd written to this partition.
# Writing an empty name ejects; the gadget refuses that while the Pi has the
# file system mounted, so power the Pi off (or unmount) first.
LUN=/sys/kernel/config/usb_gadget/g0/functions/mass_storage.disk0/lun.0

kvm_attach() {
	kvm_ssh "set -e
		test -f '$KVM_IMAGE'
		echo > $LUN/file
		echo 0 > $LUN/cdrom
		echo 0 > $LUN/ro
		echo '$KVM_IMAGE' > $LUN/file
		echo '$KVM_IMAGE' > /boot/usb.disk0
		sync
		echo \"attached: \$(cat $LUN/file) ro=\$(cat $LUN/ro) cdrom=\$(cat $LUN/cdrom)\""
}

kvm_detach() {
	kvm_ssh "set -e
		echo > $LUN/file
		: > /boot/usb.disk0
		sync
		echo \"attached: '\$(cat $LUN/file)'\""
}

fetch() {	# url, output file
	[ -s "$2" ] || { echo "fetch $1"; curl -fsSL --retry 3 -o "$2.part" "$1"; mv "$2.part" "$2"; }
}

build() {
	for tool in curl sfdisk mkfs.vfat mcopy mmd truncate ssh-keygen openssl sha256sum; do
		command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
	done
	[ -r "$PUBKEY" ] || { echo "missing public key $PUBKEY" >&2; exit 1; }

	# --- Alpine's Raspberry Pi files -------------------------------------
	local tarball=$WORK/dl/alpine-rpi-$ALPINE_VERSION-aarch64.tar.gz
	fetch "$ALPINE_MIRROR/$ALPINE_BRANCH/releases/aarch64/$(basename "$tarball")" "$tarball"
	echo "$ALPINE_SHA256  $tarball" | sha256sum -c --quiet
	local root=$WORK/build/fat
	rm -rf "$WORK/build"
	mkdir -p "$root"
	tar xzf "$tarball" -C "$root"

	# --- host apk tool, checked against the keys from the verified tarball -
	local host=$WORK/build/host
	mkdir -p "$host/keys"
	tar xzf "$root"/apks/aarch64/alpine-keys-*.apk -C "$host/keys" 2>/dev/null
	local apk_pkg=$WORK/dl/apk-tools-static-$APK_STATIC_VERSION.x86_64.apk
	fetch "$ALPINE_MIRROR/$ALPINE_BRANCH/main/x86_64/apk-tools-static-$APK_STATIC_VERSION.apk" "$apk_pkg"
	tar xzf "$apk_pkg" -C "$host" 2>/dev/null
	openssl dgst -sha256 \
		-verify "$host/keys/usr/share/apk/keys/x86_64/$APK_STATIC_KEY" \
		-signature "$host/sbin/apk.static.SIGN.RSA.sha256.$APK_STATIC_KEY" \
		"$host/sbin/apk.static" >/dev/null

	# --- extra packages for offline install -------------------------------
	local extras=$root/extras
	mkdir -p "$extras/apks" "$WORK/dl/apks" "$host/apkroot"
	local apk=("$host/sbin/apk.static" --root "$host/apkroot" --arch aarch64
		--keys-dir "$host/keys/usr/share/apk/keys/aarch64" --no-interactive
		--repository "$ALPINE_MIRROR/$ALPINE_BRANCH/main"
		--repository "$ALPINE_MIRROR/$ALPINE_BRANCH/community")
	"${apk[@]}" add --initdb --usermode --quiet
	"${apk[@]}" update --quiet
	# shellcheck disable=SC2086
	(cd "$extras/apks" && "${apk[@]}" fetch --quiet --recursive $EXTRA_PACKAGES)
	# Packages the image's own repository already has would only waste space.
	local name
	for name in "$extras"/apks/*.apk; do
		[ -e "$root/apks/aarch64/$(basename "$name")" ] && rm "$name"
	done
	echo "$EXTRA_PACKAGES" > "$extras/packages.txt"
	echo "extras: $(ls "$extras/apks" | wc -l) packages, $(du -sh "$extras/apks" | cut -f1)"

	# --- rpi-eeprom tools and Pi 4 EEPROM images ---------------------------
	local file
	for file in $RPI_EEPROM_FILES $RPI_EEPROM_EXTRA_FILES; do
		local cached=$WORK/dl/rpi-eeprom-$RPI_EEPROM_REF/$file
		mkdir -p "$(dirname "$cached")"
		fetch "https://raw.githubusercontent.com/raspberrypi/rpi-eeprom/$RPI_EEPROM_REF/$file" "$cached"
		case $file in
		firmware-2711/*) mkdir -p "$extras/eeprom/firmware-2711"
			cp "$cached" "$extras/eeprom/firmware-2711/$(basename "$file")";;
		*) mkdir -p "$extras/eeprom"; cp "$cached" "$extras/eeprom/";;
		esac
	done
	echo "$RPI_EEPROM_REF" > "$extras/eeprom/REF"

	# --- firmware configuration --------------------------------------------
	# Alpine's config.txt ends with "include usercfg.txt".
	cat > "$root/usercfg.txt" <<-'EOF'
	# rpi4 lab recovery OS
	enable_uart=1
	uart_2ndstage=1
	# PL011 (ttyAMA0) on GPIO14/15 instead of Bluetooth
	dtoverlay=disable-bt
	disable_overscan=1
	EOF
	# The last console= is /dev/console: boot progress shows on HDMI, kernel
	# messages go to both, and init adds a getty on each.
	echo "modules=loop,squashfs,sd-mod,usb-storage console=ttyAMA0,115200 console=tty1" \
		> "$root/cmdline.txt"

	# --- the overlay (apkovl) ------------------------------------------------
	local ovl=$WORK/build/ovl
	mkdir -p "$ovl"/etc/{apk,network,local.d,ssh/sshd_config.d,runlevels/boot,runlevels/default} \
		"$ovl/root/.ssh"
	: > "$ovl/etc/.default_boot_services"	# init then adds the stock services
	echo "$HOSTNAME_PI" > "$ovl/etc/hostname"
	printf '127.0.0.1\t%s localhost\n::1\tlocalhost\n' "$HOSTNAME_PI" > "$ovl/etc/hosts"
	printf '%s\n' $BASE_PACKAGES > "$ovl/etc/apk/world"
	cat > "$ovl/etc/network/interfaces" <<-'EOF'
	auto lo
	iface lo inet loopback

	auto eth0
	iface eth0 inet dhcp
	EOF
	ln -s /etc/init.d/networking "$ovl/etc/runlevels/boot/networking"
	for name in sshd chronyd local; do
		ln -s /etc/init.d/$name "$ovl/etc/runlevels/default/$name"
	done
	cat > "$ovl/etc/ssh/sshd_config.d/10-recovery.conf" <<-'EOF'
	PermitRootLogin prohibit-password
	PasswordAuthentication no
	UseDNS no
	EOF
	local keys=$STATE/recovery-hostkeys
	if [ ! -s "$keys/ssh_host_ed25519_key" ]; then
		mkdir -p "$keys"; chmod 700 "$keys"
		ssh-keygen -q -t ed25519 -N '' -C "$HOSTNAME_PI" -f "$keys/ssh_host_ed25519_key"
	fi
	install -m 600 "$keys/ssh_host_ed25519_key" "$ovl/etc/ssh/"
	install -m 644 "$keys/ssh_host_ed25519_key.pub" "$ovl/etc/ssh/"
	echo "$HOSTNAME_PI $(cut -d' ' -f1,2 "$keys/ssh_host_ed25519_key.pub")" > "$STATE/known_hosts"
	chmod 700 "$ovl/root" "$ovl/root/.ssh"
	install -m 600 "$PUBKEY" "$ovl/root/.ssh/authorized_keys"
	cat > "$ovl/etc/motd" <<-'EOF'
	rpi4 lab recovery OS (Alpine, running from RAM). Boot media: /media/<dev>,
	read-only; the SD card is /dev/mmcblk0. Setup log: /var/log/recovery-setup.log
	EOF
	cat > "$ovl/etc/local.d/10-recovery.start" <<-'EOF'
	#!/bin/sh
	# Install the extra tools from the boot media (no network needed), fall
	# back to the network, then show the address on the consoles.
	log=/var/log/recovery-setup.log
	extras=$(ls -d /media/*/extras 2>/dev/null | head -n 1)
	{
		echo "recovery setup: extras at ${extras:-<none>}"
		if [ -n "$extras" ] && apk add --no-network --no-interactive "$extras"/apks/*.apk; then
			echo "extras installed offline"
		fi
		cat >> /etc/apk/repositories <<-REPOS
		http://dl-cdn.alpinelinux.org/alpine/v$(cut -d. -f1,2 /etc/alpine-release)/main
		http://dl-cdn.alpinelinux.org/alpine/v$(cut -d. -f1,2 /etc/alpine-release)/community
		REPOS
		if ! command -v vcgencmd >/dev/null && [ -n "$extras" ]; then
			echo "offline install failed; trying the network"
			apk add --no-interactive --update-cache $(cat "$extras/packages.txt")
		fi
		if [ -d "$extras/eeprom" ]; then
			mkdir -p /usr/local/bin /usr/local/share/rpi-eeprom
			cp -r "$extras"/eeprom/firmware-2711 /usr/local/share/rpi-eeprom/
			for f in rpi-eeprom-config rpi-eeprom-update rpi-eeprom-digest; do
				install -m 755 "$extras/eeprom/$f" /usr/local/bin/
			done
		fi
	} >> $log 2>&1
	for tty in /dev/tty1 /dev/ttyAMA0; do
		[ -c $tty ] && {
			echo
			echo "=== rpi4-recovery ready: $(ip -4 -o addr show eth0 | awk '{print $4}') ==="
			tail -n 3 $log
		} > $tty 2>/dev/null
	done
	exit 0
	EOF
	chmod 755 "$ovl/etc/local.d/10-recovery.start"
	tar -C "$ovl" --owner=0 --group=0 --numeric-owner -czf \
		"$root/$HOSTNAME_PI.apkovl.tar.gz" etc root

	# --- the disk image: MBR, one FAT32 (LBA) partition at 1 MiB -------------
	local sectors=$((IMAGE_MIB * 2048))
	rm -f "$IMAGE"
	truncate -s "${IMAGE_MIB}M" "$IMAGE"
	printf 'label: dos\nstart=2048, type=c, bootable\n' | sfdisk --quiet "$IMAGE"
	mkfs.vfat -F 32 -s 8 -n RPIRECOVERY --offset 2048 "$IMAGE" $(((sectors - 2048) / 2)) >/dev/null
	export MTOOLS_SKIP_CHECK=1
	(cd "$root" && for name in * .alpine-release; do
		mcopy -i "$IMAGE@@1M" -s -Q "$name" ::
	done)
	# Read every file back out of the image and compare.
	local check=$WORK/build/check
	mkdir -p "$check"
	mcopy -i "$IMAGE@@1M" -s -Q -n '::*' "$check/"
	diff -r "$root" "$check" >/dev/null || { echo "image read-back differs" >&2; exit 1; }
	rm -rf "$check"
	sfdisk -l "$IMAGE" | tail -n 2
	sha256sum "$IMAGE" | tee "$IMAGE.sha256"
}

deploy() {
	local sum remote
	sum=$(cut -d' ' -f1 "$IMAGE.sha256")
	# /data on the NanoKVM is nearly full and shared with other projects:
	# replace only our own file, in place, after ejecting it. Compressed
	# stream (the image is mostly empty), checksum afterwards.
	kvm_ssh "set -e
		[ \"\$(cat $LUN/file)\" != '$KVM_IMAGE' ] || echo > $LUN/file
		rm -f '$KVM_IMAGE'"
	gzip -1 -c "$IMAGE" | kvm_ssh "gunzip -c > '$KVM_IMAGE' && sync"
	remote=$(kvm_ssh "sha256sum '$KVM_IMAGE'" | cut -d' ' -f1)
	[ "$remote" = "$sum" ] || { echo "upload checksum mismatch ($remote)" >&2; exit 1; }
	kvm_attach
}

case ${1:-build} in
build) build;;
deploy) build; deploy;;
attach) kvm_attach;;
detach) kvm_detach;;
*) sed -n '2,19p' "$0"; exit 2;;
esac
