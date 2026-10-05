#!/bin/sh
# Build nvm-<version>-<rev>-any.hpkg from the Haiku-patched nvm tree.
#
#   make-nvm-hpkg.sh <nvm-dir> [out-dir]      (run on Haiku; uses `package`)
#
# The package installs
#   data/nvm/                 nvm.sh, nvm-exec, bash_completion, LICENSE.md,
#                             haiku/patches/node-vNN.patch
#   data/profile.d/nvm.sh     the login-shell hook /etc/profile sources
#   documentation/packages/nvm/README.md
# which land in /boot/system/data/... as a system package, or in
# ~/config/data/... when the .hpkg is put in ~/config/packages.
set -e
NVM=$(cd "$1" && pwd)
OUT=${2:-$(pwd)}
VERSION=$(sed -n 's/^  "version": "\(.*\)",/\1/p' "$NVM/package.json")
REVISION=1
STAGE=$(mktemp -d /tmp/nvm-hpkg-XXXXXX)
mkdir -p "$STAGE/data/nvm/haiku" "$STAGE/data/profile.d" "$STAGE/documentation/packages/nvm"
cp "$NVM/nvm.sh" "$NVM/nvm-exec" "$NVM/bash_completion" "$NVM/LICENSE.md" "$STAGE/data/nvm/"
cp -r "$NVM/haiku/patches" "$STAGE/data/nvm/haiku/"
cp "$NVM/haiku/profile.d/nvm.sh" "$STAGE/data/profile.d/nvm.sh"
cp "$NVM/README.md" "$STAGE/documentation/packages/nvm/"
[ -f "$NVM/haiku/README.md" ] && cp "$NVM/haiku/README.md" "$STAGE/documentation/packages/nvm/HAIKU.md"
chmod 755 "$STAGE/data/nvm/nvm.sh" "$STAGE/data/nvm/nvm-exec"
cat > "$STAGE/.PackageInfo" <<EOI
name			nvm
version			$VERSION-$REVISION
architecture	any
summary			"Node Version Manager, with Haiku support"
description		"nvm installs and switches between Node.js versions per user \
(in \$NVM_DIR, default ~/.nvm). This build knows Haiku: it installs Haiku \
binaries from the mirror in \$NVM_HAIKU_MIRROR and can build other releases \
from source with the Haiku patches it ships. Login shells get the nvm \
command through data/profile.d/nvm.sh."
packager		"Haiku fork build <noreply@localhost>"
vendor			"nvm-sh (Haiku fork build)"
copyrights		{
	"2010 Tim Caswell and Jordan Harband"
}
licenses		{
	"MIT"
}
urls			{
	"https://github.com/nvm-sh/nvm"
}
provides		{
	nvm = $VERSION
}
requires		{
	haiku
	cmd:bash
	cmd:curl
	cmd:sha256sum
	cmd:tar
	cmd:xz
}
EOI
PKG="$OUT/nvm-$VERSION-$REVISION-any.hpkg"
rm -f "$PKG"
package create -C "$STAGE" "$PKG"
rm -rf "$STAGE"
package list "$PKG" | head -40
