# Node.js + nvm on Haiku (x86_64)

Owner request: `nvm` part of the fork's standard deploy; Node 26, 24, 23, 22,
21, 20, 18, 16 installable through it; Claude Code installable into Node 26.
Main session: read "Summary", then "Integration into the fork image".

## Summary (2026-09-27 06:40 host time)

- All 8 majors are built for Haiku x86_64 with every dependency bundled
  (OpenSSL, full ICU, libuv, c-ares, nghttp2, brotli, zlib ...) and npm +
  corepack, and VERIFIED in the build VM (the fork's anyboot ISO installed
  to disk + the nvm package) with `nvm install <major>` from a file://
  mirror: 16.20.2, 18.20.8, 20.20.2, 21.7.3, 22.23.3, 23.11.1, 24.21.0,
  26.10.0 (26 with Temporal). Last run: all 8 PASS in a fresh NVM_DIR.
- nvm v0.40.8 + a small Haiku patch, packaged as
  `pkg/nvm-0.40.8-1-any.hpkg`; verified as a system package in the VM
  (login shells get `nvm` and the default Node).
- Claude Code 2.1.112 (the last pure-JS release) installs with npm into
  Node 26, `claude --version` works and the TUI starts to its login screen
  (VM). Current releases (>= 2.1.113, latest 2.1.283) are native-only and
  cannot run on Haiku.
- WORKSTATION: nothing done there yet - it was down (main session notice:
  wait until told). `scripts/install-workstation.sh` does the whole
  install; see "Workstation".
- Decision for the owner: where to host dist/ (the binary mirror). The
  default `NVM_HAIKU_MIRROR_DEFAULT` in nvm.sh is a marked placeholder.

## Layout of this directory

| Path | What |
|---|---|
| `pkg/nvm-0.40.8-1-any.hpkg` | the nvm package for the image (built by scripts/make-nvm-hpkg.sh) |
| `pkg/ripgrep-14.0.1-1-x86_64.hpkg` | HaikuPorts ripgrep (for Claude Code), checksum = HaikuPorts repo |
| `nvm/` | nvm v0.40.8 (tag, commit a885b885) + Haiku changes in nvm.sh; nvm-exec, bash_completion, docs unchanged; install.sh is upstream's curl-installer (fetches upstream nvm from GitHub, not used on Haiku) |
| `nvm/haiku/patches/node-vNN.patch` | Haiku patch per Node major (16 18 20 21 22 23 24 26), applied by nvm for source builds |
| `nvm/haiku/profile.d/nvm.sh` | login-shell hook (goes to `<data>/profile.d/nvm.sh`) |
| `nvm/haiku/README.md` | user docs (installed as documentation/packages/nvm/HAIKU.md) |
| `dist/` | the Haiku binary mirror, nodejs.org layout: `index.tab`, `index.json`, `vX.Y.Z/node-vX.Y.Z-haiku-x64.tar.{xz,gz}`, `vX.Y.Z/SHASUMS256.txt` (595 MB) |
| `scripts/port-haiku.py` | applies the Haiku edits to a pristine Node tree; the patches are its output |
| `scripts/platform-haiku.cc` | V8 platform file, one source for V8 9.4 .. 14.6 |
| `scripts/ares/ares_config-X.Y.Z.h` | c-ares' own configure result on Haiku, per bundled c-ares |
| `scripts/make-patch.sh` | host: pristine tarball + port-haiku.py -> nvm/haiku/patches/node-vNN.patch |
| `scripts/build-node.sh` | Haiku: configure/make/install/tarball (nodejs.org layout) |
| `scripts/make-dist-index.py` | host: SHASUMS256.txt, index.json, index.tab for dist/ |
| `scripts/make-nvm-hpkg.sh` | Haiku: builds the nvm .hpkg from nvm/ |
| `scripts/test-node.js`, `verify-major.sh`, `verify-nvm-features.sh` | the verification used below |
| `scripts/pty-run.py` | runs a TUI in a pty and prints the screen (Claude Code check) |
| `scripts/install-workstation.sh` | host: installs nvm, all majors, ripgrep, Claude Code on ws-haiku |

## Integration into the fork image (for the main session)

nvm is one package, `pkg/nvm-0.40.8-1-any.hpkg` (architecture any):

| In the package | On a system install | What |
|---|---|---|
| `data/nvm/nvm.sh`, `nvm-exec`, `bash_completion`, `LICENSE.md` | `/boot/system/data/nvm/` | nvm itself (read-only is fine: all state is in $NVM_DIR) |
| `data/nvm/haiku/patches/node-v{16,18,20,21,22,23,24,26}.patch` | `/boot/system/data/nvm/haiku/patches/` | used by source builds |
| `data/profile.d/nvm.sh` | `/boot/system/data/profile.d/nvm.sh` | login hook |
| `documentation/packages/nvm/{README.md,HAIKU.md}` | `/boot/system/documentation/packages/nvm/` | docs |

Requires haiku, cmd:bash, cmd:curl, cmd:sha256sum, cmd:tar, cmd:xz (all in
the standard deploy; provided by bash, curl, coreutils, tar, xz_utils).
`$NVM_DIR` defaults to `~/.nvm` and is created at login.

Putting it into the image - pick one:
1. Ship the .hpkg as is, e.g. in UserBuildConfig:
   `AddFilesToHaikuImage system packages : /mnt/HaikuWork/x399/node-haiku/pkg/nvm-0.40.8-1-any.hpkg ;`
   This is exactly what was tested (the .hpkg moved into
   /boot/system/packages of the VM).
2. Or add the files to the haiku package (build/jam/packages/Haiku):
   `AddFilesToPackage data nvm : nvm.sh nvm-exec bash_completion LICENSE.md ;`,
   `AddFilesToPackage data nvm haiku patches : node-v16.patch ... ;`,
   `AddFilesToPackage data profile.d : <nvm-hook>nvm.sh ;` (hook and nvm.sh
   share a name - give one a different grist).
   Not AddFilesToHaikuImage for `system data`: /boot/system is packagefs,
   loose files under system/data are hidden.

No change to data/etc/profile is needed: it already sources
`findpaths -Re B_FIND_PATH_DATA_DIRECTORY profile.d`/*.sh (system first,
user last). The hook runs once and loads the most specific nvm it finds
(`~/config/non-packaged/data/nvm`, then `~/config/data/nvm`, then
`/boot/system/data/nvm`), so a user can override the system copy.

Login cost: sourcing nvm.sh in every login shell added ~3 s to a
Terminal/SSH login in the VM (under build load; 0.5 s without). The hook
therefore only puts the default version's bin/ on PATH (reads
$NVM_DIR/alias/default in pure shell, no processes) and defines an `nvm`
stub that sources nvm.sh (`--no-use`) on first use: ~0.7-1.0 s under the
same load. `nvm use` etc. then behave normally (PATH entries replaced).
Haiku's login bash reads `~/config/settings/profile` for user settings (not
~/.profile or ~/.bash_profile - checked).

Binary mirror: set `NVM_HAIKU_MIRROR` (https://... or file:///path) or edit
`NVM_HAIKU_MIRROR_DEFAULT` in nvm/nvm.sh (then rebuild the .hpkg) once the
hosting location is decided. Current default: the placeholder
`https://haiku-node-mirror.invalid/dist` (with it, `nvm install` explains
that no mirror is configured). The mirror must hold dist/ exactly (after
adding releases run `scripts/make-dist-index.py dist`). Nothing was
published anywhere.

Packaging trap: copying an .hpkg straight into a packages directory with
scp makes package_daemon read a half-written file ("failed to init
package"); copy it elsewhere on the same volume and `mv` it in.

## Per-version results (final binaries, dist/)

Verification (`scripts/verify-major.sh <major>` in a login shell in the VM,
`NVM_HAIKU_MIRROR=file:///boot/home/node-mirror` = a copy of dist/):
`nvm install <major>` (checksum verified), then scripts/test-node.js: fs
(sync/promises/rename/readdir), child_process (execSync, spawnSync of node,
exit codes), crypto (sha256, AES-256-GCM, ECDSA P-256), TLS to
https://nodejs.org/dist/index.json via https.get and via fetch (16 has no
fetch), dns lookup/resolve4, zlib gzip/deflate/brotli, Intl with full ICU
(de-DE numbers, ja-JP full date, fr-FR month, ar-EG plurals),
worker_threads, os; npm/npx --version; `npm install semver@7 ms@2` into a
scratch project and use them; and no crash reports from any child process.

| Major | Version | V8 | npm | ICU | Build (VM, -j10/12) | VM | Workstation |
|---|---|---|---|---|---|---|---|
| 16 | 16.20.2 | 9.4.146 | 8.19.4 | 71.1 | 13 min | PASS | not yet |
| 18 | 18.20.8 | 10.2.154 | 10.8.2 | 74.2 | 27 min | PASS | not yet |
| 20 | 20.20.2 | 11.3.244 | 10.8.2 | 78.2 | 13-28 min | PASS | not yet |
| 21 | 21.7.3 | 11.8.172 | 10.5.0 | 74.2 | 37 min | PASS | not yet |
| 22 | 22.23.3 | 12.4.254 | 10.9.9 | 78.3 | 26-45 min | PASS | not yet |
| 23 | 23.11.1 | 12.9.202 | 10.9.2 | 76.1 | 55 min | PASS | not yet |
| 24 | 24.21.0 | 13.6.233 | 11.19.0 | 78.3 | 30-60 min | PASS | not yet |
| 26 | 26.10.0 | 14.6.202 | 11.19.1 | 78.3 | 75 min (Temporal on) | PASS | not yet |

(Build times vary with load on the shared host.) SHA-256 of the .tar.xz:
16 f275c845..., 18 18bd3756..., 20 11669a9e..., 21 3cb27010..., 22 dd66d45d...,
23 69089000..., 24 5bbaa139..., 26 bafc4541... (full values in
dist/vX.Y.Z/SHASUMS256.txt).

nvm features (`scripts/verify-nvm-features.sh`, VM, all PASS): nvm
--version, ls-remote (Haiku releases + io.js), ls-remote --lts (Latest LTS
Krypton), version-remote 24, ls-remote 25 falling back to nodejs.org, ls,
use, current, which, run, exec, `alias default 26` picked up by a new login
shell, `npm install -g cowsay` under 26 (bin link works), install by LTS
name (lts/iron), uninstall + reinstall.

Source fallback (VM, nvm from the system package):
- `nvm install 25.9.0`, build tools hidden from PATH: not on the Haiku
  mirror -> resolved via nodejs.org's index -> binary download fails ->
  "Building Node.js from source on Haiku needs build tools that are not
  installed: gcc binutils make patch python3.10 / Install them with: pkgman
  install gcc binutils make patch python3.10".
- Same with the tools present: "There is no Haiku patch for Node.js 25.x
  (node-v25.patch), so v25.9.0 cannot be built on Haiku."
- `nvm install -s -j 10 16` (fresh NVM_DIR): downloads node-v16.20.2.tar.xz
  from nodejs.org, checksum OK, "Applying the Haiku patch
  /boot/system/data/nvm/haiku/patches/node-v16.patch", configure, make -j10,
  make install into $NVM_DIR: done in 27 min, "Now using node v16.20.2 (npm
  v8.19.4)", and the smoke test passes on the result. So the shipped patch
  works on a pristine release tarball and nvm's source path works end to
  end.

Known limits (all versions): `os.cpus()` model is "unknown" and CPU times
are 0 (libuv's Haiku backend does not read them); no native backtraces in
V8/Node fatal errors (no execinfo; Haiku's debugger still has them); native
addons cannot use Node's copy of ICU (see the ICU section); with the Time
preferences at "GMT", Node 22+ reports the zone as "+00:00" (same as Linux
Node with TZ=GMT; Node 16-21 say "UTC").

## nvm changes (nvm.sh against v0.40.8; ~110 lines)

1. `nvm_get_os`: `Haiku *` -> `haiku`. Arch detection already works
   (`uname -m` = x86_64 -> x64); make-jobs detection falls back to `nproc`
   (Haiku has no getconf); sha256sum, xz, GNU tar are in the base system.
2. Mirrors: on Haiku the version index (`index.tab`), the binaries and
   their SHASUMS256.txt come from `$NVM_HAIKU_MIRROR` (nodejs.org layout);
   source tarballs and their checksums still come from
   `$NVM_NODEJS_ORG_MIRROR`. `nvm_get_mirror` takes an optional third
   argument (binary/source), threaded through `nvm_download_artifact`,
   `nvm_get_checksum`, `nvm_ls_remote_index_tab`. Mirror URLs may be
   `file://` too (curl reads them). Placeholder default, see above.
3. `nvm_ls_remote` on Haiku lists the Haiku mirror's releases; a pattern
   matching none of them falls back to the upstream index (so a release
   without a Haiku binary can be installed, from source).
4. Source builds on Haiku: `nvm_haiku_check_build_prerequisites` (names
   the missing tools and the pkgman command), `nvm_haiku_patch` (finds
   `haiku/patches/node-vNN.patch` next to nvm.sh, in `$NVM_HAIKU_PATCH_DIR`,
   `$NVM_DIR/haiku/patches` or `/boot/system/data/nvm/haiku/patches`),
   `nvm_haiku_apply_patch` after extraction; configure gets
   `--dest-os=haiku --dest-cpu=x64`.
5. A failed binary download with no `NVM_HAIKU_MIRROR` set says so.
6. `nvm exec`/`nvm run` used `$NVM_DIR/nvm-exec`, which does not exist when
   nvm lives outside $NVM_DIR (the system package); they fall back to the
   nvm-exec next to nvm.sh.
Diff: compare nvm/nvm.sh with nvm-sh/nvm tag v0.40.8.

## Node.js port (what the patches change)

Starting point: HaikuPorts nodejs20-20.15.1 (shared system libs, no npm).
Ours bundle everything and ship npm/corepack, so a tarball needs only
libroot, libbsd, libnetwork and gcc_syslibs at run time. The patches are
generated by scripts/port-haiku.py from pristine sources (anchored,
idempotent edits; version-specific ones are optional), so a new release of
a major can be ported by rerunning it (plus a c-ares config if c-ares
changed).

All majors:
- Build system: configure accepts `--dest-os=haiku`, gyp detects the Haiku
  flavor, tools/utils.py knows Haiku; common.gypi treats Haiku like the
  other ELF/gcc POSIX systems; node.gypi links -lbsd -lnetwork,
  whole-archives V8 and OpenSSL (for addons) and hides the bundled ICU
  (below).
- libuv: uv.gyp gets libuv's own Haiku source list (haiku.c,
  bsd-ifaddrs.c, no-fsevents.c, no-proctitle.c, posix-hrtime.c,
  posix-poll.c; -lbsd -lnetwork).
- c-ares: `deps/cares/config/haiku/ares_config.h` is c-ares' cmake result
  on Haiku for each bundled version (c-ares 1.19.1 needed
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 with cmake 4).
- OpenSSL: Node's headers pick a per-arch config by preprocessor; Haiku
  now selects the Linux ones (OPENSSL_LINUX also for __HAIKU__; the macro is
  only used by Node's config headers). Without it the 32-bit linux-elf
  config was used and the x86_64 bignum asm failed. openssl_common.gypi:
  OPENSSLDIR=/boot/system/data/ssl, no -ldl, -lnetwork.
- Node: skip SIGKILLTHR when resetting signal dispositions (HaikuPorts had
  commented the CHECKs out); no execinfo; node_report skips
  RLIMIT_MEMLOCK/RSS/NPROC. HaikuPorts' os.cpus() hack is not carried.
- ICU time zone: TZ is unset and there is no /etc/localtime on Haiku, so
  Node ran in "Etc/Unknown" (UTC) whatever the Time preferences said; the
  patched uprv_tzname() asks the kernel (`_kern_get_timezone`, as libroot
  does). Verified: kernel zone Europe/Berlin -> Intl zone Europe/Berlin,
  `new Date(0)` 01:00 CET; TZ still overrides.
- V8: V8_OS_HAIKU; platform-haiku.cc (thread_info.stack_end is the stack
  start - HaikuPorts returns stack_base, the low end, which is wrong;
  shared libraries listed via get_next_image_info); no sys/syscall.h;
  sampler reads rip/rsp/rbp from Haiku's mcontext; export-template.h hack
  (Haiku's gcc predefines __declspec); trap-handler __thread where V8 still
  has the AIX case.

Version-specific:
- 16: a second signal-reset loop at exit (node_main_instance.cc) - every
  process aborted at exit before; npm 8's cacache moves cache entries with
  link(), and BFS has no hard links (EPERM) -> rename instead.
- 21: QUIC deps (ngtcp2/nghttp3) only included <arpa/inet.h> on Linux ->
  htonl() etc. were undefined.
- 22: common_node.gypi's C++20 OS list gets Haiku (ncrypto uses <=>).
- 22+: abseil GetTID() uses find_thread() (pthread_t is a pointer).
- 24+: no IPV6_TCLASS -> setTypeOfService() on IPv6 returns an error;
  OpenSSL 3.5 configs link "-lm -ldl -pthread"; V8 13+/14's "local-exec"
  TLS for g_current_isolate_ cannot link because Haiku links executables
  like shared objects -> V8's shared-library TLS mode.
- 26: Temporal enabled: the vendored Rust crates are built with rust_bin
  1.94.1 (needs rustc/cargo >= 1.86, only for building); the staticlib's
  std needs -lnetwork -lbsd.

ICU symbol clash (found on 21, affected 18 and 21): npm hung at start.
libroot's tzset()/localtime()/gmtime() load the locale backend, which loads
Haiku's own ICU (libicuuc.so.74) into the process; Node exports all its
symbols (-rdynamic, for addons) and 18/21 bundle ICU 74, so the system
ICU's calls bound to Node's copy (same names, e.g. u_init_74) and it
deadlocked on a lock Node's ICU held. Fix in all patches:
`-Wl,--exclude-libs,libicudata.a:libicui18n.a:libicuucx.a` keeps the bundled
ICU out of the dynamic symbol table (Node binds it at link time). This also
protects 16/20/22/23/24/26 if Haiku's ICU version changes. All 8 final
binaries were built (or relinked) with it and re-verified.

## Claude Code

Latest (`latest` = 2.1.283, 2026-09-26): does not work on Haiku.
- `npm install -g @anthropic-ai/claude-code` exits 0 but installs only a
  stub (bin/claude.exe + install.cjs); the per-platform
  optionalDependencies exist for darwin/linux/linux-musl/win32 only.
- Node 26's npm 11.19 does not run its postinstall by default ("install
  scripts not yet covered by allowScripts"); with
  `--allow-scripts=@anthropic-ai/claude-code --foreground-scripts` (or npm
  10 under Node 20, which runs it but hides the output) it prints
  `[@anthropic-ai/claude-code postinstall] Unsupported platform: haiku x64`
  and lists darwin-arm64, darwin-x64, linux-x64, linux-arm64,
  linux-x64-musl, linux-arm64-musl, linux-arm64-android,
  linux-x64-android, win32-x64, win32-arm64 (2.1.197 also listed FreeBSD).
- Running `claude` then prints "Error: claude native binary not installed
  ..." and exits 1. Only a Haiku build from Anthropic would change that; no
  native package was unpacked or modified.

2.1.112 (last pure-JS release, cli.js, engines node>=18): works.
- VM, Node 26.10.0 (final binary): `npm install -g
  @anthropic-ai/claude-code@2.1.112` -> `claude --version` = `2.1.112
  (Claude Code)` (also on Node 24).
- In a pty (scripts/pty-run.py, TERM=xterm-256color): logo and "Let's get
  started. Choose the text style ..." render; Enter leads to "Select login
  method: Claude account with subscription / Anthropic Console account /
  3rd-party platform". No credentials used.
- Needs ripgrep: its vendored rg has no Haiku build -> HaikuPorts ripgrep
  14.0.1-1 plus `USE_BUILTIN_RIPGREP=0`; and `DISABLE_AUTOUPDATER=1` so it
  does not try to switch to a native build. Its optional sharp image
  codecs, audio-capture and seccomp binaries have no Haiku builds (image
  paste, voice and sandboxing will not work).

## Workstation (not done yet)

The workstation was down (main session notice) and has not been touched.
When it is back, `scripts/install-workstation.sh` (run on the build host):
1. copies pkg/nvm-0.40.8-1-any.hpkg to `~/config/packages` (a user package:
   gives ~/config/data/nvm + ~/config/data/profile.d/nvm.sh; nothing under
   /boot/system) - once the image ships nvm this can be removed;
2. copies dist/ to `~/node-dist` and installs 16 18 20 21 22 23 24 26 with
   `NVM_HAIKU_MIRROR=file:///boot/home/node-dist nvm install <major>`;
3. `nvm alias default 26` - chosen over 24 because Claude Code has to live
   in Node 26's global prefix and only the default version's bin/ is on
   PATH in new shells; 26 also becomes LTS in October 2026. 24 (current
   LTS) stays one `nvm use 24` away;
4. installs pkg/ripgrep-14.0.1-1-x86_64.hpkg into `~/config/packages`;
5. `npm install -g @anthropic-ai/claude-code@2.1.112` under Node 26 and
   checks `claude --version`; the exports DISABLE_AUTOUPDATER=1 and
   USE_BUILTIN_RIPGREP=0 still need to go into ~/config/settings/profile.

## Build environment and traps

- VM: QEMU/KVM on the build host, 12 vCPU, 12 GB (20 GB was OOM-killed by
  the shared host, 16 GB tight), q35, AHCI raw disk (one BFS volume, no
  partition table, `makebootable`), e1000 user net, ssh on 127.0.0.1:2223.
  Scratchpad `node-vm/` (start.sh, ssh_config host `hvm`, disk.raw ~11 GB).
- Installed from build/x86_64/haiku-nightly-anyboot.iso (hrev60097+26):
  live boot, mkfs -t bfs, copy system/packages, system/settings, home,
  makebootable. Added HaikuPorts packages already on this host: gcc 13.3,
  binutils 2.46.1, mpc, mpfr, gcc_syslibs_devel, make, python3.10 (+ libffi,
  file, file_data), cmake (+ rhash, libuv), ninja, pkgconfig, patch,
  haiku_devel _26, rust_bin 1.94.1 (26's Temporal).
- VM debug_server: `default_action report` (crashes go to ~/Desktop as
  reports instead of an alert that blocks the parent forever).
- Haiku facts used: sys.platform `haiku`; no getconf; gcc -pthread OK; no
  libdl/librt; sigaction fails only for SIGKILL/SIGSTOP/SIGKILLTHR;
  madvise, malloc_usable_size, getentropy exist; no pthread_getattr_np,
  sys/syscall.h, ucontext.h; thread_info.stack_base is the low end.
- Traps met: `_kern_set_timezone` shifts the system clock (broke make
  timestamps once); a restarted build's log got a stray line from the
  previous make; one Node 23 link used a stale V8 startup snapshot
  ("SyntaxError: Unexpected token" in node_mksnapshot) and one Node 24
  compile command came out truncated - both went away by re-running make
  (not port bugs); the fork's kernel panicked once on `shutdown` in TCP
  EndpointManager::Unbind ("bound endpoint not in hash", sshd teardown;
  vm-shutdown-kdl-tcp-panic.png) - a network-stack bug for the main
  session, unrelated to Node.

## Status log

- 07:00 (09-27): nvm source build (`nvm install -s 16`) verified; user-package
  install of nvm (~/config/packages, as planned for the workstation) checked.
- 06:40 (09-27): all 8 final binaries verified (fresh NVM_DIR), nvm
  features verified, final .hpkg in pkg/.
- 03:10: ICU clash fixed and confirmed on 21; all majors rebuilt/relinked
  with the fix and re-verified by 06:30.
- 00:45: 26 with Temporal verified; Claude Code 2.1.112 reaches the login
  screen in the VM.
- 21:25-23:00 (09-26): 16 exit abort + npm hard links fixed; 24 TLS/IPV6/
  OpenSSL fixes; VM OOM-killed once, restarted smaller.
- 20:45: first binary (20.20.2) verified through nvm; nvm packaged.
- 19:00: VM installed from the fork's ISO.
