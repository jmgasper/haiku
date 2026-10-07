# First-boot storage setup

The `rpi4-airos` SD image includes `apps/rpi-installer` from the workspace,
packaged as `rpi_installer` / `/boot/system/apps/RPiInstaller`. On the first
desktop start, `data/boot/rpi/UserBootscript` launches it with `--first-boot`.
It offers to use the entire SD card for air/OS. Keeping the current size is
a persistent choice; closing setup or choosing Later leaves it available
next boot. Applications → Raspberry Pi Setup can revisit the choice.

The app identifies the partition mounted at `/boot`, validates the specific
MBR FAT32 + BFS image layout and its growable BFS superblock, and displays
the current and available capacities. It requires explicit confirmation
before changing the partition table. A backup, decision and pending state
live under `~/config/settings/rpi-installer/`. It flushes and reads back the
new partition table, then offers a normal restart. Completion is recorded
only after the enlarged mounted filesystem is observed on the next boot.

## Why the image has reserved metadata

General BFS resizing is still experimental and cannot safely relocate open
inodes on a running boot volume. This image uses a separate, opt-in format
parameter: `block_size 4096; growable true`. It reserves 64 MiB for the bitmap
and 16 MiB for the journal, enough for the MBR limit of 2 TiB. Those blocks
are marked allocated from creation; all normal file data starts after them.

After the user extends the partition, the next mount replays the journal
and grows BFS before publishing vnodes or initializing its allocator. It
clears only bitmap bits beyond the old filesystem size, flushes the device,
then commits the new block and allocation-group counts. Existing files,
inodes, indexes and journal locations do not move. Partly prepared bitmap
space can be cleared again on retry while the original size remains valid.
The operation never shrinks, never grows an ordinary BFS volume, and never
writes during a read-only mount. The general resize ioctl is disabled for
this reserved layout so it cannot reclaim the metadata reservation.

The MMC driver implements the flush completion barrier for SD cards by
waiting for CMD13 to report READY_FOR_DATA in transfer state. MMC cards with
an enabled cache also perform their existing explicit cache flush.

## Building a distributable image

Build the installer package, stage the application's packages (including the
desired Summit and summit_webkit revisions), then build `rpi4-airos`:

```sh
bash tools/airos/build-arm64-app-packages.sh \
  /mnt/HaikuWork/rpi4/packages-arm64 rpi_installer
bash tools/rpi4/stage-packages.sh
jam -q -j6 @rpi4-airos build airos-rpi-image
```

In the build directory's `UserBuildConfig`, set `RPI4_LAB_IMAGE = 0 ;`
before including `tools/rpi4/UserBuildConfig`. This excludes lab credentials
and services, regardless of the local lab-image marker. An optional
`RPI4_IMAGE_PACKAGES` points at an isolated staging directory. The image
build fails if its package directory has no rpi_installer package. The
minimum bring-up profile retains its existing format and startup behavior.

## Verification

The app's `make check` validates capacities, arithmetic boundaries, unrelated
partitions, boot-device identity, malformed superblocks, already-full cards,
and repeat expansion. `tests/test_bfs_growth.py` runs the
actual BFS implementation through the host `bfs_shell`: 128 GB, 128 GiB and
2 TiB growth, partial bitmap blocks/bytes, retried preparation, original-file
hashes, a new file larger than the original filesystem, remounts, read-only
mounts, ordinary BFS, rejected versions/sizes, and `checkfs -c`.

Native validation uses a disposable copy of the complete Pi image on a
128 GiB QEMU disk. An EFI loader added only to that copy permits the `virt`
machine's USB input to exercise the UI. The production FAT contents remain
the Pi firmware boot path. Hardware validation on a physical 128 GB card
is separate from these emulator and filesystem checks.

The first native run on 2026-10-07 passed the initial prompt, decline and
reboot without a repeat prompt, manual reopening, cancel without changes,
confirmed expansion, graceful restart, growth from 3.50 GiB to 127.75 GiB,
and a further boot without a repeat prompt. After shutdown, `checkfs -c`
reported 435 nodes and zero missing, duplicate or reclaimable blocks. The
partition backup matched the original image MBR byte for byte and the
installed app package's SHA-256 was unchanged. Evidence lives under
`/mnt/HaikuWork/artifacts/rpi-installer/qemu/`; the filesystem and planner
test logs are beside it. These checks used the validation image while the
latest Summit rebuild was in progress.

The merged Pi kernel also passed growth through QEMU's `raspi4b` SD
controller: `/dev/disk/mmc/0/1` mounted at 137166323712 bytes after the SD
flush barriers succeeded. The setup window reported 127.75 GiB and a host
`checkfs -c` after stopping the VM reported 418 nodes with no missing,
duplicate or reclaimable blocks (`sd-current.log`, `sd-current.png`, and
`sd-current-checkfs.log` in the same artifact directory).

The build server's `airos-rpi4` profile uses the same growable format and
requires `rpi_installer` in `AIROS_CI_PACKAGES`. Its image pipeline selects
this Pi-only package from the ARM64 package pool.

A validation image (`505134964a`, 2026-10-07) included the build server's fresh
Summit `60f37cb` packages: `summit-0.1.0~git20261007.0055-1-arm64` and
`summit_webkit-1.10.1~git20261007.0055-1-arm64`. The package hashes extracted
from the image match the staged copies. Its installer expanded a decimal
128 GB disk to 127727370240 filesystem bytes (118.96 GiB) and restarted
successfully. Summit rendered its start page and a local JavaScript test.
The final Pi SD-controller boot also displayed first-boot setup. Evidence
is in `final-qemu/`, `final-sd.log`, `final-sd.png`, and `final-verification/`.

An incremental build crossing the Pi optimization merge can retain old
merged libc objects or a Zstd reader compiled without `ZSTD_ENABLED`.
Rebuild the stale aggregates/readers or use a clean output directory. The
final image was tested after rebuilding those objects; no application
package failed to load. The compressed deliverable and source/package
manifest are in the artifact directory's `release/` subdirectory.

The delivered image uses Haiku `b6854ab9f8` and the newer completed Summit
build `cfbe5f5` (`git20261007.0112-1` packages, including certificate details).
Its fresh boot displayed setup, loaded all packages, and rendered a local
JavaScript test. All 42 package hashes extracted from the image matched
their expected inputs. The xz stream expands to the exact raw-image SHA-256.
The final evidence is in `latest-qemu/`, `latest-verification/`, and the
release manifest. The physical Pi test remains separate.
