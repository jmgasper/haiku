# nvm on Haiku

This nvm (v0.40.8 plus Haiku support) is loaded into every login shell
(Terminal, SSH) by `profile.d/nvm.sh`. Node.js versions are installed per
user into `$NVM_DIR`, `~/.nvm` unless set otherwise.

    nvm install 24        # newest 24.x that has a Haiku build
    nvm install 26
    nvm alias default 24
    nvm use 26
    nvm ls-remote         # releases with Haiku binaries

## Where the binaries come from

nodejs.org does not publish Haiku builds. On Haiku, nvm reads the release
index, the `node-vX.Y.Z-haiku-x64.tar.xz` archives and their
`SHASUMS256.txt` from the mirror in `NVM_HAIKU_MIRROR`, a directory in the
nodejs.org layout, served over https or read locally:

    export NVM_HAIKU_MIRROR=https://example.org/node-haiku/dist
    export NVM_HAIKU_MIRROR=file:///boot/home/node-dist

Source tarballs still come from nodejs.org (`NVM_NODEJS_ORG_MIRROR`).

## Releases without a Haiku binary

If a version is not on the Haiku mirror, `nvm ls-remote <version>` and
`nvm install <version>` fall back to the nodejs.org index, and nvm builds the
release from source: it applies `haiku/patches/node-vNN.patch` for the major
version and runs configure with `--dest-os=haiku`. That needs

    pkgman install gcc binutils make patch python3.10 haiku_devel

(nvm names whatever is missing) and takes about 15 minutes on 12 fast cores,
much longer on slower machines. `nvm install -s <version>` forces a source
build. Patches exist for Node.js 16, 18, 20, 21, 22, 23, 24 and 26; each was
made for the release named in its first line and usually applies to the other
releases of the same major.
