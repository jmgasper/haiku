#!/bin/bash
# Stage a haiku.hpkg that is the one installed on the workstation with only
# some files replaced by local builds, for changes that should reach the
# machine without everything else the branch has gained since that package
# was built (kernel, drivers and all).
#
# usage: repack-haiku-pkg.sh <package path>=<local file> ...
#   e.g. servers/app_server=$OBJ/servers/app/app_server
#
# The package is unpacked and packed again on the workstation itself, so that
# every file keeps its attributes (a Linux file system would drop them), and
# replaced files are overwritten in place so that they keep theirs. The result
# is /boot/home/x399-stage/haiku.hpkg; install-staged-haiku-pkg.sh puts it in
# place and power cycles.
set -euo pipefail
X399=${X399:-/mnt/HaikuWork/x399}
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
STAGE=/boot/home/x399-stage

[ $# -gt 0 ] || { echo "usage: $0 <package path>=<local file> ..." >&2; exit 2; }

$SSH "rm -rf $STAGE && mkdir -p $STAGE/files"
for pair in "$@"; do
	target=${pair%%=*}
	source=${pair#*=}
	[ -f "$source" ] || { echo "no such file: $source" >&2; exit 1; }
	flat=$(echo "$target" | tr / _)
	$SSH "cat > $STAGE/files/$flat" < "$source"
	remote=$($SSH "sha256sum $STAGE/files/$flat" | cut -d' ' -f1)
	[ "$remote" = "$(sha256sum "$source" | cut -d' ' -f1)" ] \
		|| { echo "upload of $source was damaged" >&2; exit 1; }
done

$SSH "set -e
	name=\$(ls /boot/system/packages/ | grep '^haiku-r1')
	mkdir -p $STAGE/root
	package extract -C $STAGE/root /boot/system/packages/\$name
	for pair in $*; do
		target=\${pair%%=*}
		flat=\$(echo \$target | tr / _)
		[ -f $STAGE/root/\$target ] || { echo \"the package has no \$target\" >&2; exit 1; }
		cat $STAGE/files/\$flat > $STAGE/root/\$target
	done
	package create -C $STAGE/root -i $STAGE/root/.PackageInfo $STAGE/haiku.hpkg > /dev/null
	package list /boot/system/packages/\$name | awk '{print \$1, \$2}' | sort > $STAGE/before.txt
	package list $STAGE/haiku.hpkg | awk '{print \$1, \$2}' | sort > $STAGE/after.txt
	echo 'files that differ from the installed package:'
	diff $STAGE/before.txt $STAGE/after.txt | grep '^>' || true
	rm -rf $STAGE/root"
echo "staged $STAGE/haiku.hpkg"
