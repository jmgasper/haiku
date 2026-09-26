#!/bin/bash -l
# Verify one Node.js major on Haiku through nvm, the way a user would:
#
#   verify-major.sh <major> [test-node.js]
#
# Runs in a login shell (so the profile.d hook has loaded nvm), installs
# the major from $NVM_HAIKU_MIRROR with `nvm install`, runs the smoke test,
# and installs a pure-JS package with npm into a scratch project.
set -u
MAJOR=$1
TEST=${2:-$(dirname "$0")/test-node.js}
echo "== nvm $(nvm --version) on $(uname -srm), NVM_DIR=$NVM_DIR"
echo "== NVM_HAIKU_MIRROR=${NVM_HAIKU_MIRROR-}"
echo "== nvm install $MAJOR"
nvm install "$MAJOR" || { echo "RESULT $MAJOR: nvm install FAILED"; exit 1; }
nvm use "$MAJOR" >/dev/null
echo "== node $(node --version) at $(command -v node), npm $(npm --version), npx $(npx --version)"
fail=0
# Crashed programs leave debug_server reports on the Desktop (the VM sets
# default_action report); a crash in a child process must not go unnoticed.
reports_before=$(ls ~/Desktop/*.report 2>/dev/null | wc -l)
node "$TEST" || fail=1
PROJ=$(mktemp -d /tmp/npmtest-XXXXXX)
(
	cd "$PROJ" &&
	npm init -y >/dev/null &&
	npm install --no-audit --no-fund semver@7 ms@2 &&
	node -e "const semver = require('semver'), ms = require('ms');
if (!semver.satisfies(process.version, '>=$MAJOR.0.0 <$((MAJOR + 1)).0.0')) throw new Error('semver');
if (ms('2h') !== 7200000) throw new Error('ms');
console.log('PASS npm install: semver ' + require('semver/package.json').version + ', ms ' + require('ms/package.json').version);"
) || { echo "FAIL npm install"; fail=1; }
rm -rf "$PROJ"
reports_after=$(ls ~/Desktop/*.report 2>/dev/null | wc -l)
if [ "$reports_after" != "$reports_before" ]; then
	echo "FAIL crash reports: $((reports_after - reports_before)) new in ~/Desktop"
	fail=1
else
	echo "PASS no crashes (no new debug_server reports)"
fi
if [ $fail = 0 ]; then echo "RESULT $MAJOR: PASS ($(node --version))"; else echo "RESULT $MAJOR: FAIL ($(node --version))"; fi
exit $fail
