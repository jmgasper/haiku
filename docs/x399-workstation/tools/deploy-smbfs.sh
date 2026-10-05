#!/bin/bash
# Build the smbfs and userland_fs packages and put them on the workstation.
#
# Both are packages of their own, so nothing has to be restarted for them: the
# package daemon takes the old ones out and the new ones in. What Tracker and
# the mount_server have to do with shares is in the haiku package; that goes
# the way of repack-haiku-pkg.sh.
#
# Three things that are easy to get wrong, all of them silently:
#
# - A userlandfs server stays when its volume is unmounted, to be there for
#   the next one, and with it stays the add-on it has loaded. So the servers
#   are ended here, which is only possible while no share is mounted.
# - The version does not change with the contents, and so neither does the
#   name of the package file. A package that is replaced by one of its own
#   name before the package daemon has taken it out stays as it was.
# - smbfs needs userland_fs. Taking userland_fs out first makes the package
#   daemon ask on the screen whether smbfs is to go as well, and wait for the
#   answer.
set -euo pipefail
X399=${X399:-/mnt/HaikuWork/x399}
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
PACKAGES=$X399/build/x86_64/objects/haiku/x86_64/packaging/packages

cd $X399/build/x86_64
source $X399/tools/env.sh
jam -q userland_fs.hpkg smbfs.hpkg 2>&1 | grep -E "error|\.\.\.failed" || true

if $SSH "df | grep -q userlandfs"; then
	echo "unmount the shares first" >&2
	exit 1
fi
# the team's ID is the fourth column from the right; the command line has
# columns of its own
$SSH 'for team in $(ps | grep "[u]serlandfs_server smbfs" \
			| awk "{print \$(NF-3)}"); do
		kill $team || true
	done'

for package in userland_fs smbfs; do
	[ -f $PACKAGES/$package.hpkg ] || { echo "$package did not build" >&2; exit 1; }
	$SSH "cat > /boot/home/$package.new.hpkg" < $PACKAGES/$package.hpkg
	remote=$($SSH "sha256sum /boot/home/$package.new.hpkg" | cut -d' ' -f1)
	[ "$remote" = "$(sha256sum $PACKAGES/$package.hpkg | cut -d' ' -f1)" ] \
		|| { echo "upload of $package was damaged" >&2; exit 1; }
done

$SSH 'set -e
	addon=/system/add-ons/userlandfs/smbfs
	server=/system/servers/userlandfs_server

	wait_for() {
		# wait_for <file> <yes|no>: until it is there, or is not
		for i in $(seq 60); do
			if [ -e $1 ]; then there=yes; else there=no; fi
			[ $there = $2 ] && return 0
			sleep 1
		done
		echo "the package daemon did not get to $1, is it asking something" \
			"on the screen?" >&2
		return 1
	}

	rm -f /boot/system/packages/smbfs-*.hpkg
	wait_for $addon no
	rm -f /boot/system/packages/userland_fs-*.hpkg
	wait_for $server no

	for package in userland_fs smbfs; do
		version=$(package list -i /boot/home/$package.new.hpkg \
			| grep "version:" | awk "{print \$2}")
		mv /boot/home/$package.new.hpkg \
			/boot/system/packages/$package-$version-x86_64.hpkg
		if [ $package = smbfs ]; then
			wait_for $addon yes
		else
			wait_for $server yes
		fi
		echo installed $package $version
	done
	sync'
