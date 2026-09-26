#!/bin/sh
# Install nvm, the Node.js versions and Claude Code on the X399 workstation
# (run on the build host). Nothing under /boot/system is touched:
#   - nvm goes in as a *user* package (~/config/packages/nvm-*.hpkg), which
#     gives ~/config/data/nvm and ~/config/data/profile.d/nvm.sh;
#   - the binary mirror is copied to ~/node-dist and used as file://;
#   - Node.js versions go to ~/.nvm (NVM_DIR);
#   - ripgrep (HaikuPorts) also goes in as a user package;
#   - Claude Code 2.1.112 is npm-installed into Node 26's global prefix, and
#     its two settings are exported from ~/config/settings/profile (which
#     Haiku's login bash reads).
#
#   install-workstation.sh [majors...]      (default: 16 18 20 21 22 23 24 26)
set -e
N=/mnt/HaikuWork/x399/node-haiku
CFG=/mnt/HaikuWork/x399/ssh/config
SSH="ssh -F $CFG ws-haiku"
MAJORS=${*:-16 18 20 21 22 23 24 26}
HPKG=$(ls "$N"/pkg/nvm-*-any.hpkg | tail -n 1)
RG=$N/pkg/ripgrep-14.0.1-1-x86_64.hpkg

# $1 = local .hpkg: copy into ~/config/cache, then mv it into packages (a
# half-copied file in a packages directory fails to activate)
user_package() {
	scp -q -F "$CFG" "$1" ws-haiku:config/cache/"$(basename "$1").part"
	$SSH "mkdir -p ~/config/packages && mv ~/config/cache/$(basename "$1").part ~/config/packages/$(basename "$1")"
}

echo "== nvm package $(basename "$HPKG") -> ~/config/packages"
$SSH 'rm -f ~/config/packages/nvm-*.hpkg'
user_package "$HPKG"
$SSH 'for i in $(seq 30); do [ -f ~/config/data/profile.d/nvm.sh ] && break; sleep 2; done; ls ~/config/data/nvm'

echo "== mirror -> ~/node-dist"
$SSH 'mkdir -p ~/node-dist'
tar -C "$N/dist" -cf - . | $SSH 'tar -xmf - -C ~/node-dist && cut -f1,10 ~/node-dist/index.tab'

for m in $MAJORS; do
	echo "== nvm install $m"
	$SSH "NVM_HAIKU_MIRROR=file:///boot/home/node-dist bash -lc 'nvm install $m'"
done
$SSH "bash -lc 'nvm alias default 26 && nvm ls --no-colors'"

echo "== ripgrep -> ~/config/packages"
user_package "$RG"
$SSH 'for i in $(seq 30); do command -v rg >/dev/null && break; sleep 2; done; rg --version | head -n 1'

echo "== Claude Code settings in ~/config/settings/profile"
$SSH 'P=~/config/settings/profile; touch $P; grep -q "USE_BUILTIN_RIPGREP" $P || cat >> $P <<EOF

# Claude Code 2.1.112 on Haiku: use the ripgrep package, never self-update
# to the native builds (there are none for Haiku).
export USE_BUILTIN_RIPGREP=0
export DISABLE_AUTOUPDATER=1
EOF
tail -n 5 $P'

echo "== Claude Code 2.1.112 into Node 26"
$SSH "bash -lc 'nvm use 26 >/dev/null && npm install -g @anthropic-ai/claude-code@2.1.112 && command -v claude && claude --version'"
