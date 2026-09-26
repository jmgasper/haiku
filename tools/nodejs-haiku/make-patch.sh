#!/bin/sh
# make-patch.sh X.Y.Z : extract pristine node-vX.Y.Z to a temp dir, run
# port-haiku.py with PORT_HAIKU_ORIG, write nvm/haiku/patches/node-vMAJOR.patch
# from plain diffs, and leave the patched tree at $SP/node-src/node-vX.Y.Z.
set -e
SP=${SP:?set SP to a scratch dir with official/vX.Y.Z/node-vX.Y.Z.tar.xz and ares/ares_config-X.Y.Z.h}
V=$1; M=${V%%.*}
OUT=${OUT:-/mnt/HaikuWork/x399/node-haiku/nvm/haiku/patches/node-v$M.patch}
cd "$SP/node-src"
rm -rf "node-v$V" "orig-$V"
tar xJf "official/v$V/node-v$V.tar.xz"
PORT_HAIKU_ORIG="$SP/node-src/orig-$V" python3 /mnt/HaikuWork/x399/node-haiku/scripts/port-haiku.py "node-v$V" "$SP/ares/ares_config-$V.h"
{
	echo "Haiku (x86_64) port of Node.js v$V, all dependencies bundled."
	echo "Made by node-haiku/scripts/port-haiku.py (+ c-ares' configure result on Haiku); apply with patch -p1."
	echo
	(cd "orig-$V" && find . -type f | sed 's|^\./||' | sort) | while read -r f; do
		case "$f" in
			*.NEW) f=${f%.NEW}; diff -u --label /dev/null --label "b/$f" /dev/null "node-v$V/$f" || true ;;
			*) diff -u --label "a/$f" --label "b/$f" "orig-$V/$f" "node-v$V/$f" || true ;;
		esac
	done
} > "$OUT"
rm -rf "orig-$V"
echo "$OUT: $(grep -c '^+++ ' "$OUT") files, $(wc -c < "$OUT") bytes"
