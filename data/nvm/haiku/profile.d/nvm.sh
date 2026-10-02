# nvm, the Node Version Manager, for Haiku login shells (Terminal runs
# "sh -l"; SSH sessions are login shells too). /etc/profile sources the
# profile.d/*.sh of every data directory, the system one first. The first
# copy of this file to run sets up the most specific nvm installed: a user's
# ~/config/non-packaged/data/nvm wins over /boot/system/data/nvm.
#
# Node.js versions, aliases and caches live in $NVM_DIR (default ~/.nvm).
# Binaries come from $NVM_HAIKU_MIRROR (https://... or file:///path).
#
# Loading nvm.sh costs a few seconds on Haiku, so a login shell only puts the
# default Node.js (`nvm alias default ...`) on PATH; the real nvm is loaded
# the first time the nvm command is used.
if [ -z "${NVM_HAIKU_PROFILE_LOADED-}" ]; then
	NVM_HAIKU_PROFILE_LOADED=1
	NVM_HAIKU_SH=$(findpaths -e B_FIND_PATH_DATA_DIRECTORY nvm/nvm.sh 2>/dev/null | head -n 1)
	if [ -n "$NVM_HAIKU_SH" ]; then
		export NVM_DIR="${NVM_DIR:-$HOME/.nvm}"
		[ -d "$NVM_DIR" ] || mkdir -p "$NVM_DIR"

		# Resolve the default alias to the newest matching installed version,
		# without starting any process.
		_nvm_a=
		[ -r "$NVM_DIR/alias/default" ] && read -r _nvm_a < "$NVM_DIR/alias/default"
		for _nvm_i in 1 2 3 4 5; do
			case "$_nvm_a" in
				node | stable) _nvm_a='*'; break ;;
				[a-z]*)
					if [ -r "$NVM_DIR/alias/$_nvm_a" ]; then
						read -r _nvm_a < "$NVM_DIR/alias/$_nvm_a"
					else
						_nvm_a=; break
					fi ;;
				*) break ;;
			esac
		done
		_nvm_a=${_nvm_a#v}
		_nvm_best= _nvm_bmaj=-1 _nvm_bmin=-1 _nvm_bpat=-1
		if [ -n "$_nvm_a" ]; then
			for _nvm_v in "$NVM_DIR"/versions/node/v$_nvm_a "$NVM_DIR"/versions/node/v$_nvm_a.*; do
				[ -x "$_nvm_v/bin/node" ] || continue
				_nvm_n=${_nvm_v##*/v}
				_nvm_maj=${_nvm_n%%.*}; _nvm_r=${_nvm_n#*.}
				_nvm_min=${_nvm_r%%.*}; _nvm_pat=${_nvm_r#*.}
				case "$_nvm_maj$_nvm_min$_nvm_pat" in *[!0-9]*) continue ;; esac
				if [ "$_nvm_maj" -gt "$_nvm_bmaj" ] || { [ "$_nvm_maj" -eq "$_nvm_bmaj" ] && {
					[ "$_nvm_min" -gt "$_nvm_bmin" ] || { [ "$_nvm_min" -eq "$_nvm_bmin" ] && [ "$_nvm_pat" -gt "$_nvm_bpat" ]; }; }; }; then
					_nvm_best=$_nvm_v _nvm_bmaj=$_nvm_maj _nvm_bmin=$_nvm_min _nvm_bpat=$_nvm_pat
				fi
			done
		fi
		if [ -n "$_nvm_best" ]; then
			export PATH="$_nvm_best/bin:$PATH"
			export NVM_BIN="$_nvm_best/bin"
			export NVM_INC="$_nvm_best/include/node"
		fi
		unset _nvm_a _nvm_i _nvm_v _nvm_n _nvm_r _nvm_maj _nvm_min _nvm_pat _nvm_best _nvm_bmaj _nvm_bmin _nvm_bpat

		# The nvm command: load the real one on first use.
		nvm() {
			unset -f nvm
			. "$NVM_HAIKU_SH" --no-use
			nvm "$@"
		}
		if [ -n "${BASH_VERSION-}" ] && [ -s "${NVM_HAIKU_SH%/nvm.sh}/bash_completion" ]; then
			. "${NVM_HAIKU_SH%/nvm.sh}/bash_completion"
		fi
	fi
fi
