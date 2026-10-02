#!/bin/sh
# Refresh the workstation's package repository cache by hand.
#
# `pkgman refresh` fails on this build with "Operation not supported": it asks
# the mirror for repo.checksum, which the mirrors no longer publish (they serve
# repo.sha256 now). pkgman is happy with a cache file that was put there by
# other means, so fetch the index with curl and drop it in place.
#
# Run this on the workstation, not here.
set -e

CACHE=/system/cache/package-repositories
HAIKUPORTS=https://haikuports-repository.cdn.haiku-os.org/master/x86_64/current

curl -fsSL -o "$CACHE/HaikuPorts.new" "$HAIKUPORTS/repo"
mv -f "$CACHE/HaikuPorts.new" "$CACHE/HaikuPorts"
echo "HaikuPorts index refreshed: $(wc -c < "$CACHE/HaikuPorts") bytes"

# pkgman's own downloader hits the same wall, so installs need the .hpkg
# fetched first:  curl -fsSLO $HAIKUPORTS/packages/<name>-<version>-x86_64.hpkg
#                 pkgman install ./<name>-<version>-x86_64.hpkg
