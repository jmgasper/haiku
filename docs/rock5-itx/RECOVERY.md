# Recovery for this lab

This unit is a ROCK 5 ITX, PCB v1.12, with a 7,818,182,656-byte eMMC user
area and 16,777,216-byte SPI NOR. The workstation USB-C connection reaches
the RK3588 USB download interface, vendor/product `2207:350b`. NanoKVM is
independent of this USB connection and supplies reset, keyboard and UART
capture. Keep all backup files and tool logs under `/mnt/HaikuWork`.

The local state files are authoritative for completed tests:

- `state/rock5-backup.json`: original images, filesystem cleanup of a separate
  restoration copy, checksums, and restoration evidence.
- `state/rock5-maskrom.json`: independent RAM-loader access to eMMC and SPI.
- `state/rock5-restore-drill.json`: full write/read-back and ROOBI boot result.
- `state/rock5-uefi-trial.json`: native EFI and F4 recovery evidence, if tested.
- `state/rock5-efi-recovery.json`: F4 entry and the ordinary reset attempt.
- `state/rock5-maskrom-exit.json`: RAM-loader reset and standalone EFI recovery.

Do not infer that the entire recovery procedure passed from USB enumeration
or a checksum alone. In particular, software entry to MaskROM is separate
from the physical-button fallback when boot firmware cannot initialize.

## Backup contents

The original eMMC user-area image includes both GPT copies, the boot firmware,
the `config` and `boot` FAT volumes, and the ROOBI ext4 root. The snapshot was
copied into target RAM while root was frozen and `/config` was read-only,
then transferred to the workstation and checked against its source hash.
An independent timeout process could thaw the target if the snapshot process
failed. The target returned to normal writable operation after copying.

Both 4 MiB eMMC hardware boot areas and the entire SPI device were saved and
hashed separately. The boot areas are distinct from partitions in the user
area. RPMB is not included; this is a boot/OS restoration procedure, not a
backup of provisioned secure-storage keys.

Offline checks found dirty flags on the two FAT volumes and three deleted,
still-open ext4 inodes in the snapshot. The original image was retained.
Automatic filesystem cleanup was applied only to separate copies; those
copies passed subsequent read-only checks and were inserted into a separate
full restoration image. The original small FAT32 `config` geometry is retained.
Every correction and resulting checksum is recorded in the backup manifest.

The existing SPI contents begin with an XZ signature and do not constitute a
verified standalone boot firmware image. Original ROOBI boots from eMMC.
Preserve the original SPI bytes as part of restoring the observed configuration.

## USB tool and storage selection

The tested tool is Rockchip `rkdeveloptool` source revision
`304f073752fd25c854e1bcf05d8e7f925b1f4e14`, built locally at
`/mnt/HaikuWork/build/rkdeveloptool/rkdeveloptool`. This revision supports
explicit storage selection and reads the selection back to confirm it.
Other packaged versions have different command names and capabilities.

The Radxa-published RAM loader is `rk3588_spl_loader_v1.15.113.bin`, SHA-256
`26baab70e6b915364f7d73d88298366db1bfc346e34683e95d3d11b52492047f`.
Its `db` operation loads recovery code into RAM; it does not install new
firmware onto eMMC or SPI.

From the original vendor ROOBI boot path, `sudo reboot loader` entered the USB loader.
With exactly one RK3588 download device connected:

```sh
source /mnt/HaikuWork/src/haiku/tools/rock5-itx/env.sh
rktool=/mnt/HaikuWork/build/rkdeveloptool/rkdeveloptool
"$rktool" ld
"$rktool" rd 3
# Wait for ld to report Maskrom before downloading the RAM loader.
"$rktool" ld
"$rktool" db /mnt/HaikuWork/artifacts/firmware/rk3588_spl_loader_v1.15.113.bin
"$rktool" cs 1
"$rktool" rfi
```

Storage `1` is eMMC and must report **15,269,888 sectors**. Storage `9` is
SPI NOR and must report **32,768 sectors**. Both use 512-byte sectors in the
download protocol. Confirm capacity after every selection and check image
size and checksum before writing. Do not use the blanket erase command.

Under the hardware lock with UART capture active, the tested restoration
operation writes the selected full image at sector zero (`wl 0 IMAGE`),
reads the complete selected storage back (`rl 0 SECTORS READBACK`), and
compares SHA-256 before resetting. Restore eMMC and SPI as distinct operations.
Use the exact manifest paths, not an image selected by filename alone.
The full eMMC image does not restore the separate hardware boot areas.

After both read-backs match, reset through NanoKVM and require a fresh ROOBI
SSH boot ID. Save raw serial output, tool logs and hashes together. A normal
ROOBI boot changes mounted filesystem contents, so perform byte-for-byte
verification while still in the download loader, before starting Linux.

## Firmware trials and fallback

The EDK2 project recommends SPI storage and requires removing competing
U-Boot firmware. The prepared trial retains eMMC sectors 0–63, including
the primary GPT, and clears only sectors 64–32767 in the reserved region
before the first OS partition. Restoring the original first 16 MiB reverses
that change. Restoring the saved full SPI image reverses the firmware change.
The full eMMC backup remains available if OS partitions also need restoration.

EDK2's F4 shortcut into MaskROM passed on this unit after enabling NanoKVM's
documented [BIOS keyboard mode](https://wiki.sipeed.com/hardware/en/kvm/NanoKVM/faq.html).
Create `/boot/BIOS` on NanoKVM and restart the controller; the ordinary USB
rebind script does not rebuild its HID descriptors. The controller restart
expired this unit's API session, which then needed renewal. Preserve the
selected recovery image in `/boot/usb.disk0` before restarting NanoKVM.

F4 evidence is in `artifacts/firmware-trials/recovery-check-20260911T073121Z-476943`.
The following reset/power-button recovery attempt remained in MaskROM. Loading
the verified RAM downloader with `db`, checking readiness with `td`, and issuing
`rd 0` returned to EDK2 and the standalone ROOBI EFI launcher. No storage writes
were needed for that exit. The downloaded RAM loader still reports `Maskrom`
in `ld`; do not require the displayed mode to change to `Loader` as a readiness
test. Serial output and the download protocol prove that the RAM loader ran.
Successful exit evidence is in
`artifacts/firmware-trials/maskrom-exit-20260911T074004Z-ce46e1`, with ROOBI
boot ID `0c0d415f-504d-49ef-bbb1-7fbb879e197b` and `/sys/firmware/efi` present.

The current Haiku firmware profile uses `ConfigTableMode=2` (device tree only)
and `FdtCompatMode=2` (mainline), both UINT32 variables under GUID
`10f41c33-a468-42cd-85ee-7043213f73a3`. The release defaults to table mode 3
(ACPI plus DT), which made this Haiku loader enumerate sixteen CPU entries
on the eight-core board. Table mode 2 reports eight. The original variable
bytes and verified change are retained in `state/efi-config-before-dt-only.json`
and `state/rock5-dt-only.json`. Linux efivarfs required temporarily clearing
the immutable flag on this one variable, writing attributes plus value with
an unbuffered binary stream, and restoring that flag. ROOBI recovery has passed
with the resulting profile. Reapply the profile after reinstalling EDK2 with
default NVRAM; restoring factory boot firmware is a separate configuration.

If firmware cannot reach keyboard initialization,
use the board's physical MaskROM button as described in the
[Radxa ROCK 5 ITX recovery instructions](https://docs.radxa.com/en/rock5/rock5itx/low-level-dev/maskrom/linux).
The USB cable and RAM-loader protocol are shared by both entry routes.

The workstation currently uses a narrowly scoped, temporary udev rule in
`/run/udev/rules.d/70-haiku-rockchip.rules`, granting the `plugdev` group access
to `2207:350b`. It expires when the workstation reboots. The prepared local
installer is `nanokvm/tools/enable-rockchip-usb.sh`; running it with workstation
sudo is a one-time host setup step, independent of target deployments.

## SD-card Debian recovery OS (2026-09-19)

The owner installed Radxa Debian 11 (`5.10.110-37-rockchip`, user `radxa`
with sudo) on an SD card (`mmcblk1`; the eMMC stays `mmcblk0`). The
installation, most likely run through ROOBI (Radxa's installer, whose root
filesystem `b055efba…` lives on the eMMC), also changed the board:

- **SPI:** it replaced EDK2 v1.1 in the 16 MiB SPI flash with Radxa U-Boot.
  The overwritten flash is saved in `artifacts/spi-inspection/`. EDK2 v1.1
  was written back from Debian through `/dev/mtdblock0`
  (`artifacts/firmware/rock-5-itx_UEFI_Release_v1.1-spi16MiB.img`), and the
  read-back matches `a54d4474…`.
- **EDK2 settings:** the restore also brought back EDK2's default NVRAM.
  `ConfigTableMode` went back to 3 (ACPI plus DT), and Haiku hung right after
  ExitBootServices. It was set to 2 again from Debian through efivarfs, as
  above (`state/rock5-dt-only-reapplied-20260919.json`).
- **SD loader:** EDK2's own SPL tries the SD card first ("Trying to boot from
  MMC2") and started the SD's U-Boot FIT. The SD's loader area (sectors
  64–32767, idbloader and u-boot.itb, before the first partition at 32768)
  was saved to
  `artifacts/debian-sd-recovery/20260919T071535Z-fd86c8/sd-loader-sectors-64-32767.bin`
  and zeroed. Writing it back restores the SD's standalone boot.
- **eMMC:** it rewrote the eMMC loader area and wrote to ROOBI's root. The
  Mesa readbacks therefore compare with a new baseline,
  `state/linux-emmc-regions-baseline.json`.

The recovery image is now `debian-sd-recovery`: the lab's ROOBI EFI loader
starts Debian's own EFI-stub kernel, initrd and `rk3588-rock-5-itx.dtb`,
with `root=UUID=9e383de3-…` on the SD card and `acpi=off`. `state/lab.json`
has `recovery_ssh` set to `rock5-debian` (key login; the sudo password is in
`nanokvm/.debian-sd-password`, mode 0600) and keeps the ROOBI image as
`roobi_fallback_image`. Debian came up in 66 s and stayed stable
(`artifacts/debian-sd-recovery/20260919T071535Z-fd86c8/result.json`).

**Finding: data corruption during I/O on Debian under EDK2.** While
streaming the 300 MB eMMC partition over SSH, some attempts produced a wrong
board-side SHA-256 and others a corrupted copy on the lab host. Several
different wrong values appeared, while the eMMC data itself (hashed with
coreutils, and with direct and buffered reads) was always right. A 2 GiB
pattern scan over a minute and 30 in-memory hashes of 64 MiB showed nothing.
The cause is not yet known. Candidates are the BSP kernel booting under EDK2
instead of Radxa U-Boot, and firmware-left device DMA. Readbacks therefore
retry. A pass requires the board hash, the host copy's hash and the expected
value to agree, which corruption cannot produce.
