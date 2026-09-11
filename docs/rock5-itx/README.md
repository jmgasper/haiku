# Haiku on ROCK 5 ITX

This is jmgasper's experimental ARM64 board fork. The immediate deliverable is
a reproducible build and a remotely controlled hardware lab. Native ROCK boot
and driver support are tracked separately in [STATUS.md](STATUS.md) and the
[hardware roadmap](ROADMAP.md). This is not a fully supported Haiku image yet.
The [GitHub work items](TRACKING.md) split the roadmap into issues and milestones.

This fork uses AI-assisted development at its owner's request. Upstream Haiku
does not accept AI-assisted contributions. The `rock5-itx` branch contains this
work; `master` is retained as an upstream baseline. There is no upstream PR.

## Workspace

All local work goes on the mounted `/mnt/HaikuWork` filesystem. The scripts fail
if the mount is missing. They use existing system tools but do not install onto
the main drive or use Docker storage. Default parallelism is eight jobs.

| Directory under `/mnt/HaikuWork` | Contents |
| --- | --- |
| `src/haiku` | This fork |
| `src/buildtools` | Pinned Haiku cross-toolchain sources, including GCC dependencies |
| `toolchains/bin`, `toolchains/host` | Jam and locally extracted host utilities |
| `build/arm64` | ARM64 compiler, packages, generated files and incremental build |
| `artifacts` | Build logs, immutable images, manifests, QEMU and hardware evidence |
| `cache`, `tmp` | Host package downloads, Python caches and temporary files |
| `state` | Local lab configuration, API session and operation locks |
| `nanokvm` | Initial evaluation, SSH identity/config, recovery snapshots and evidence |

Read the [upstream build guide](https://www.haiku-os.org/guides/building/) for host
requirements. This workstation already has GCC/G++, make, bison, flex, texinfo,
autoconf/automake, nasm, wget, unzip, xorriso, QEMU ARM64 firmware and the zlib,
zstd, curl and OpenSSL development libraries. `mtools` was downloaded with
`apt-get download` into `cache/debs` and extracted using `dpkg-deb -x` into
`toolchains/host`; no system installation was needed. Buildtools bundles its
GMP, MPFR, MPC and ISL sources.

## Build

Run these commands in Bash. The lock prevents simultaneous builds sharing the
same output directory. The source lock is [sources.json](../../tools/rock5-itx/sources.json).

```sh
cd /mnt/HaikuWork/src/haiku
source tools/rock5-itx/env.sh
bash tools/rock5-itx/check.sh
bash tools/rock5-itx/build.sh
python3 tools/rock5-itx/lab.py artifact /mnt/HaikuWork/build/arm64/haiku-arm64-mmc.image
```

The final command prints a manifest and saves a `.json` next to the immutable
`.img` in `artifacts/images`. Use that JSON path below. It records SHA-256, size,
source and toolchain revisions, host tools and downloaded package checksums.
Commit source changes before release builds; dirty development builds are
identified explicitly. This is revision-pinned reconstruction, not a claim of
bit-for-bit reproducibility across host distributions or build dates.

The image uses Haiku's existing `@minimum-mmc` recipe with a 300 MiB BFS volume
and an EFI partition containing `EFI/BOOT/BOOTAA64.EFI`. It can be presented as
a USB disk. The target filename ending in `.image` becomes `.img` when packaged
because NanoKVM 2.4.3 lists `.img` and `.iso` files.

For an upstream update, fetch `upstream`, merge the chosen revision into a
topic branch based on `rock5-itx`, update `sources.json` when changing the
toolchain, rebuild, and run both QEMU and hardware gates before updating the
known-good state. Do not rebuild cross-tools over a compiler used by an active
build. Preserve failed artifacts when investigating regressions.

## QEMU and hardware trials

For a complete iteration from an active development session, run:

```sh
bash tools/rock5-itx/iterate.sh
```

This checks the tools, builds, packages, requires the QEMU first-login serial
marker without a kernel panic, and then runs a hardware trial with recovery.
Hardware results remain observations until their milestone evidence is reviewed.
Individual stages are also available:

```sh
python3 tools/rock5-itx/lab.py qemu /mnt/HaikuWork/artifacts/images/IMAGE.json --el2 --seconds 90
python3 tools/rock5-itx/lab.py doctor
python3 tools/rock5-itx/lab.py cycle /mnt/HaikuWork/artifacts/images/IMAGE.json --seconds 60
python3 tools/rock5-itx/lab.py recover
```

Replace `IMAGE.json` with the emitted manifest filename. QEMU uses its own
copy-on-write overlay, a saved firmware copy, serial log, screenshot and JSON
result. `--expect REGEX` makes a missing serial marker fail the command; choose
a marker that proves the milestone under test. Without a marker the result is
`observed`, requiring review of the evidence. A loader banner is not a desktop
pass, and emulated PCI/USB is not RK3588 platform validation.

`--el2` enables virtualization and GICv3 in QEMU, exercising the VHE handoff and
EL2 physical timer used on the ROCK. The complete iteration uses this profile.
Omit it to check the EL1 path separately. Current hardware boots start all eight
CPUs, mount the NanoKVM disk through native platform EHCI, and display a basic
Tracker/Deskbar desktop. NanoKVM keyboard and mouse input, USB RNDIS networking
and authenticated remote commands also work. Stress acceptance and remaining
hardware are tracked separately in [STATUS.md](STATUS.md).

`--usb-controller ehci` puts QEMU's boot disk on a PCI EHCI controller to test
the shared EHCI transfer code. The default remains xHCI. In this profile HID
devices stay on a separate xHCI controller because QEMU's standalone EHCI has
no low/full-speed companion. Native FDT attachment and noncoherent DMA still
require a hardware trial.

`cycle` owns the hardware lock, verifies local and remote image hashes, attaches
the image in USB disk mode, resets the target, captures HDMI frames, and then
returns the board to ROOBI even if the trial or capture fails. A failed recovery
returns a nonzero status and preserves the error. Initial trials start from
reachable ROOBI. Its SSH boot ID establishes recovery; the NanoKVM power LED
API currently reports false even when the board is on.

`deploy` performs upload and attachment without rebooting. It uploads through
a temporary HTTP endpoint bound to the workstation's LAN address, serving
exactly one artifact, then shuts the server down. An existing image is never
overwritten. Each deployment gets a new writable copy on the NanoKVM so guest
writes cannot affect a later trial or the local immutable artifact. The minimum
image currently fails on write-protected USB media in QEMU, so media cannot
simply be marked read-only. Use separate scratch disks for driver stress tests.
If NanoKVM is exporting the entire `/data` partition, first ensure
it is unmounted on the ROCK and detach it before writing to the image library.
Do not edit a selected image in place. Retain the recovery image and latest
known-good image when clearing old uploads for space.

The ROCK's debug UART2 header is now connected to NanoKVM UART1 (`/dev/ttyS1`),
with TX/RX crossed and a common ground. At 1,500,000 baud, 8N1 without flow
control, boot output is readable, but longer input is corrupted; see
[STATUS.md](STATUS.md). Local SSH capture checks and raw evidence live under
`/mnt/HaikuWork/nanokvm/tools/test_uart.py` and `artifacts/serial/` respectively.
`cycle` now uses this remote serial path by default, checks that capture is ready
before deployment, and treats an SSH or UART disconnection as a failed trial.
Raw bytes, worker diagnostics and capture metadata are saved with each run.
An actual deploy/reset/recovery trial captured 30,556 bytes without transport errors.

`state/lab.json` was initialized from [lab.example.json](../../tools/rock5-itx/lab.example.json).
Set `serial_remote_device` to `/dev/ttyS1` for the NanoKVM connection. If the
ordered USB UART is later connected to this workstation and verified, clear
`serial_remote_device` and set `serial_device` to its stable
`/dev/serial/by-id/...` path. Configure only one transport. Both paths capture
a raw `serial.log` from before deployment through recovery and restore the
previous terminal settings on normal shutdown. The workstation UART path is
covered by PTY tests; the physical USB adapter has not yet been tested.

SSH credentials, host keys and API cookies remain local. After an expired API
session, run `python3 tools/rock5-itx/nanokvm.py login` to renew it; that command
prompts for the web account password. Existing NanoKVM keyboard/mouse commands
use `nanokvm/.venv/bin/python` with `websocket-client` installed.

GitHub Actions exercises control logic with mocked hardware. Builds and actual
device access run here on the workstation. There is no unattended public
self-hosted GitHub runner. Use these commands from an active development
session; the scripts do not create an independent background coding service.

## Authenticated lab sessions

The lab profile also installs `rock5_memory_probe` under
`/boot/home/config/non-packaged/bin`. For example, `rock5_memory_probe 8192 8 2`
checks a locked 8 GiB allocation with eight workers and two passes. It rejects
allocations larger than 75% of currently free RAM. `--inject-error` as the fourth
argument deliberately corrupts one word and must produce a failing exit status.
This is a short integrity diagnostic, not a sustained qualification workload.

`UserBootscript` reports CPU, RAM, USB and network inventory to `/dev/dprintf`.
An authenticated lab shell requires a separate private image overlay containing
both `home/config/settings/rock5-lab/enable-shell` and a generated password hash.
The ordinary build contains neither credential nor opt-in file. The private
listener binds to the RNDIS USB address, and the workstation reaches it through
NanoKVM SSH forwarding. Keep the overlay, credentials and its image local.

Create an overlay, validate it, and start a native session with:

```sh
python3 tools/rock5-itx/shell_image.py BASE_MANIFEST.json --rndis-only
python3 tools/rock5-itx/qemu_shell.py PRIVATE_MANIFEST.json --memory --platform --services --transfer --power --normal \
    --result /mnt/HaikuWork/state/shell-qemu.json
/mnt/HaikuWork/nanokvm/.venv/bin/python tools/rock5-itx/session.py \
    PRIVATE_MANIFEST.json /mnt/HaikuWork/state/shell-qemu.json --seconds 1800
```

Use the manifest path printed by each preceding step. `--rndis-only` blocks the
competing ECM configuration on QEMU's `usb-net` device. The QEMU profile uses four
CPUs, EHCI boot storage, xHCI input/network devices, and localhost forwarding on
an isolated virtual network. `--power` tests a software reboot, fresh login and
power-off; `--normal` exercises desktop shutdown. Omit `--normal` for the quick
kernel shutdown path. `--el1` checks HVC instead of the default EL2/SMC path.
The shell client currently uses Python 3.12's standard-library telnet support.
`--platform` checks pinned CPU clocks, fork/exec and protected-page faults.
`--services` forces reverse-ordered pipe descriptors and checks a negative
control. `--transfer` verifies an 8 MiB binary round trip and rejects truncated
input.

`session.py` owns the hardware lock, captures UART/HDMI, verifies the deployment
and consumes one JSON command per line. Run it with an interactive stdin. For
example, using the target address actually reported by that boot:

```json
{"action":"shell","target":"10.239.6.146","commands":"/mnt/HaikuWork/tmp/check.sh"}
{"action":"upload","target":"10.239.6.146","source":"/mnt/HaikuWork/build/probe","name":"probe","executable":true}
{"action":"capture"}
{"action":"finish"}
```

Command files run with `set -e`; exit status, partial output and connection errors
are saved under the session's artifact directory. Uploads are limited to 16 MiB
per file, use a temporary destination, and require matching SHA-256 before
installation under `/boot/home/rock5-lab`. A packaged native transfer program
streams binary data through NanoKVM SSH and its private USB network; the
NanoKVM needs no additional file copy. The bootstrap Bash lacks `/dev/tcp`
support. The slower terminal/base64 upload is available with
`"transport":"terminal"`. Executing a program is a separate shell
command. `finish`, EOF or the session deadline returns the board to ROOBI. Image
deployment still uses the full USB image; individual test programs can now be
built, transferred and run without rebooting.

When no session owns the hardware lock, the same operations are available as
`shell.py run TARGET COMMAND_FILE --output TRANSCRIPT` and
`shell.py upload TARGET SOURCE --name NAME --output TRANSCRIPT [--executable]`.
For a checked download, use `shell.py download TARGET NAME DESTINATION
--output TRANSCRIPT`, or the session command
`{"action":"download","target":"10.239.6.146","name":"probe","destination":"/mnt/HaikuWork/artifacts/probe-returned"}`.
The destination must be new, and downloaded bytes must match the guest checksum.
Downloads now finish receiving the file into a unique NanoKVM `/data` scratch
file before copying it to the workstation over SSH. The two stages check the
byte count and SHA-256; the final file must also match the guest's checksum.
Successful copies remove the scratch file. Failures retain any scratch file and
record its exact path in the transcript's `.staging.json` evidence. Downloads
have the same 16 MiB limit as uploads. Staged reception now defaults to
256 KiB/second with a small TCP receive window; `--rate-limit 0` (or
`"rate_limit":0`) disables this experimental pacing. Unpaced staging also
reproduced a native outage after reboot, so staging alone is not a reliable fix.
The diagnostic `--transport relay` option
(or `"transport":"relay"` in a session) retains the simultaneous USB/SSH path,
which repeatedly made this NanoKVM unreachable during native Haiku downloads.
All file arguments must be beneath `/mnt/HaikuWork`. During an active session,
use its JSON commands so target operations remain serialized.

For controller-outage testing on this NanoKVM, `guarded_session.py` accepts the
same arguments and JSON commands as `session.py`. It arms the verified hardware
watchdog only for that session and disarms it on normal exit. The web password
is read from `NANOKVM_PASSWORD` or a prompt and kept in process memory for API
reauthentication after a controller reset. Lost SSH heartbeats leave the timer
armed; recovery waits for a new controller boot ID and API readiness, starts a
fresh UART capture, then restores ROOBI. Failed trials remain failures even when
recovery succeeds. No persistent controller startup service is installed.
The watchdog recovered earlier outages, but a later relay failure left the
controller unreachable beyond the recovery deadline. A physical controller
power cycle remains necessary when that recovery route fails.

## EFI firmware and recovery

Board-specific [EDK2 v1.1](https://github.com/edk2-porting/edk2-rk3588/releases/tag/v1.1)
is installed in SPI. The native EFI diagnostic completed with a 1920x1080 GOP
framebuffer, a memory map including RAM above 4 GiB, and both device-tree and
ACPI tables. Those firmware interfaces provide the starting point for Haiku;
kernel drivers remain separate work after `ExitBootServices`.
The current Haiku profile exposes only the mainline device tree
(`ConfigTableMode=2`, `FdtCompatMode=2`), avoiding duplicate CPU enumeration
through both firmware interfaces. The original diagnostic captured both tables.

ROOBI now boots through a small EFI launcher using its original Linux kernel,
initrd and vendor DTB, with `acpi=off`. The selected recovery image is recorded
in the local lab configuration. Recovery has returned over SSH through this
EFI path. Use a 180-second recovery timeout for this configuration.

NanoKVM requires its documented `/boot/BIOS` flag and a controller restart for
EDK2 keyboard input. F4 entry into USB MaskROM has passed. Exiting MaskROM on
this unit required the verified RAM downloader followed by `rkdeveloptool rd 0`;
the normal reset and power-button sequence did not clear that state. See
[RECOVERY.md](RECOVERY.md) before firmware recovery.

Build the EFI diagnostic and recovery launcher with:

```sh
bash tools/rock5-itx/build-efi-tools.sh
python3 tools/rock5-itx/efi_media.py efi-probe \
    /mnt/HaikuWork/build/efi-tools/efi-probe.efi
```

`efi_media.py` creates a fresh 96 MiB FAT USB image and a deployment manifest,
then verifies every payload by reading it back from FAT. Add sibling files with
`--file DESTINATION=SOURCE`. The diagnostic saves `haiku-*` files on its volume
and optionally starts `recovery.efi`. For ROOBI, use `roobi-efi.efi` as the entry
and supply `roobi-kernel.efi`, `roobi-initrd.img`, `roobi.dtb`, and
`roobi-options.txt`; preserve the exact kernel/root UUID/options in the local
recovery manifest. The native table dumps and observed firmware settings are
recorded in [STATUS.md](STATUS.md).

Haiku normally requests 115,200 baud, but this EDK2 release cannot change its
serial attributes. The fork now retains the working firmware interface and UART
configuration when that request is rejected. Current ROCK trials therefore use
1,500,000 baud for both `serial_trial_baud` and `serial_baud`. The optional trial
override remains available for firmware that accepts a different rate. Capture
acknowledges each transition and records its byte offset, restores terminal
settings when it ends, and still recovers the target after failed baud control.

The owner identified this board as PCB v1.12. A full 7,818,182,656-byte eMMC
user-area backup, both 4 MiB eMMC boot areas and the 16 MiB SPI image now live
under `artifacts/recovery/`. A separately preserved restoration copy passes
offline filesystem checks. USB loader and MaskROM reads match the saved boot
region and SPI hashes. A full USB write/read-back/ROOBI-boot restoration drill has passed; see
[RECOVERY.md](RECOVERY.md) and `state/rock5-backup.json` for the procedure
and evidence.
Reliable serial command entry remains phase 1 work.
Netboot remains optional; NanoKVM already removes physical USB image swapping.
