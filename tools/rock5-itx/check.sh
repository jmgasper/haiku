#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/env.sh"
bash -n "$HAIKU_SOURCE"/tools/rock5-itx/*.sh
python3 -m unittest discover -s "$HAIKU_SOURCE/tools/rock5-itx" -p 'test_*.py' -v
git -C "$HAIKU_SOURCE" diff --check
