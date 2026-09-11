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

The image uses Haiku's existing `@minimum-mmc` recipe with a 600 MiB BFS volume
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
python3 tools/rock5-itx/lab.py qemu /mnt/HaikuWork/artifacts/images/IMAGE.json --seconds 90
python3 tools/rock5-itx/lab.py doctor
python3 tools/rock5-itx/lab.py cycle /mnt/HaikuWork/artifacts/images/IMAGE.json --seconds 60
python3 tools/rock5-itx/lab.py recover
```

Replace `IMAGE.json` with the emitted manifest filename. QEMU uses its own
copy-on-write overlay presented as a read-only USB disk, a saved firmware copy,
serial log, screenshot and JSON
result. `--expect REGEX` makes a missing serial marker fail the command; choose
a marker that proves the milestone under test. Without a marker the result is
`observed`, requiring review of the evidence. A loader banner is not a desktop
pass, and emulated PCI/USB is not RK3588 platform validation.

`cycle` owns the hardware lock, verifies local and remote image hashes, attaches
the image in read-only USB disk mode, resets the target, captures HDMI frames, and then
returns the board to ROOBI even if the trial or capture fails. A failed recovery
returns a nonzero status and preserves the error. Initial trials start from
reachable ROOBI. Its SSH boot ID establishes recovery; the NanoKVM power LED
API currently reports false even when the board is on.

`deploy` performs upload and attachment without rebooting. It uploads through
a temporary HTTP endpoint bound to the workstation's LAN address, serving
exactly one artifact, then shuts the server down. An existing image is never
overwritten. Read-only USB presentation keeps guest writes from changing the
uploaded artifact; use separate scratch disks for persistence/driver tests.
If NanoKVM is exporting the entire `/data` partition, first ensure
it is unmounted on the ROCK and detach it before writing to the image library.
Do not edit a selected image in place. Retain the recovery image and latest
known-good image when clearing old uploads for space.

`state/lab.json` was initialized from [lab.example.json](../../tools/rock5-itx/lab.example.json).
When the USB UART is connected to this workstation and verified, set
`serial_device` to its stable `/dev/serial/by-id/...` path. `cycle` will then
capture a raw `serial.log` from before deployment through recovery at the
configured baud rate. This path is implemented but physical serial capture
has not been validated without the cable.

SSH credentials, host keys and API cookies remain local. After an expired API
session, run `python3 tools/rock5-itx/nanokvm.py login` to renew it; that command
prompts for the web account password. Existing NanoKVM keyboard/mouse commands
use `nanokvm/.venv/bin/python` with `websocket-client` installed.

GitHub Actions exercises control logic with mocked hardware. Builds and actual
device access run here on the workstation. There is no unattended public
self-hosted GitHub runner. Use these commands from an active development
session; the scripts do not create an independent background coding service.

## Next hardware gate

The board currently boots vendor U-Boot into ROOBI. Establish an ARM64 EFI
launch path and early serial capture before kernel bring-up. The
[RK3588 EDK2 project](https://github.com/edk2-porting/edk2-rk3588) lists ROCK 5 ITX
as supported, but this firmware has not been installed or tested on this unit.
Its firmware peripheral support does not supply Haiku kernel drivers after
`ExitBootServices`. Decide ACPI versus device-tree handoff from actual tables
and Haiku support, and retain the bootable ROOBI recovery path.

The original SPI dump and first 16 MiB of eMMC are local snapshots, not a full
backup or validated restore image. Serial cable installation, a complete
backup/restore drill and exact board revision identification are phase 1 work.
Netboot remains optional; NanoKVM already removes physical USB image swapping.
