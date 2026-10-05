#!/usr/bin/env bash
# Cross-build the owner's three applications into arm64 .hpkg packages on the
# Linux workstation. The upstream tools/package-haiku.sh scripts run on Haiku
# and target x86_64; this mirrors them with the cross toolchain, retargets the
# package to arm64 and drops the lib:/cmd: requirements that the board's Haiku
# has no packages for (the shared libraries live in
# /boot/system/non-packaged/lib, which is on the default library search path).
set -euo pipefail
source "$(dirname "$0")/env.sh"

WORK=/mnt/HaikuWork
APPS=$WORK/apps
TOOLS=$WORK/build/arm64/objects/linux/x86_64/release/tools
STRIP=$WORK/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-strip
OUT=${1:-$WORK/rock5-image-extras/packages}
mkdir -p "$OUT"

retarget_package_info() {
	# $1 source .PackageInfo, $2 destination
	awk '
		/^architecture / { print "architecture arm64"; next }
		/^requires \{/   { print; print "\thaiku >= r1~beta6"; inreq = 1; next }
		inreq && /^\}/   { print; inreq = 0; next }
		inreq            { next }
		                 { print }
	' "$1" > "$2"
}

stage_common() {
	# $1 app source dir, $2 binary name, $3 package-info name, $4 stage
	local src=$1 binary=$2 info=$3 stage=$4
	mkdir -p "$stage/apps" "$stage/data/deskbar/menu/Applications" \
		"$stage/boot/post-install" "$stage/documentation/packages/$5"
	cp "$src/build-arm64/$binary" "$stage/apps/$binary"
	chmod 755 "$stage/apps/$binary"
	"$STRIP" --strip-debug "$stage/apps/$binary"
	# GNU strip drops the appended Haiku resources; put them back.
	"$TOOLS/xres" -o "$stage/apps/$binary" "$src/build-arm64/$binary.rsrc"
	# packagefs serves file attributes from the package, and Tracker/Deskbar read
	# the icon and signature from attributes, not from the appended resources.
	# On Haiku mimeset does this; here resattr copies the resources across.
	"$TOOLS/resattr/resattr" -O -o "$stage/apps/$binary" "$src/build-arm64/$binary.rsrc"
	retarget_package_info "$src/resources/$info" "$stage/.PackageInfo"
	cp "$src/README.md" "$src/LICENSE" "$stage/documentation/packages/$5/"
	ln -s "../../../../apps/$binary" "$stage/data/deskbar/menu/Applications/$binary"
}

build_one() {
	local src=$1 binary=$2 info=$3 pkgname=$4 script=$5
	local stage
	stage=$(mktemp -d "$HAIKU_WORK/tmp/hpkg-$pkgname-XXXXXX")
	trap 'rm -rf -- "$stage"' RETURN
	stage_common "$src" "$binary" "$info" "$stage" "$pkgname"
	cp "$src/resources/$script" "$stage/boot/post-install/$pkgname.sh"
	chmod 755 "$stage/boot/post-install/$pkgname.sh"
	if [ "$pkgname" = amp ]; then
		mkdir -p "$stage/data/Amp" "$stage/data/licenses"
		cp "$src/vendor/fontawesome/FontAwesome6Free-Solid-900.otf" "$stage/data/Amp/"
		cp "$src/vendor/fontawesome/SIL-OFL-1.1.txt" "$stage/data/licenses/SIL OFL 1.1"
		cp "$src/vendor/fontawesome/CC-BY-4.0.txt" "$stage/data/licenses/CC BY 4.0"
	fi
	local version file
	version=$(awk '$1 == "version" { print $2; exit }' "$stage/.PackageInfo")
	file="$OUT/$pkgname-$version-arm64.hpkg"
	rm -f "$file"
	"$TOOLS/package/package" create -C "$stage" "$file" >/dev/null
	printf '%s\n' "$file"
}

build_one "$APPS/tasamp"     Amp        Amp.PackageInfo        amp        amp-post-install.sh
build_one "$APPS/kiri"       Kiri       Kiri.PackageInfo       kiri       kiri-post-install.sh
build_one "$APPS/turbochook" TurboChook TurboChook.PackageInfo turbochook turbochook-post-install.sh
