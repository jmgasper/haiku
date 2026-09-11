# Tested status

Updated 2026-09-11. This page distinguishes lab readiness from native Haiku
support. No hardware row in the roadmap is accepted merely because Linux or
firmware supports it.

| Area | Evidence and state |
| --- | --- |
| Repository | `jmgasper/haiku`, `rock5-itx` branch; upstream base `855b5d0e3126c86acc84d09f8e859272019bbbc2` |
| Build tools | Haiku GCC 13.3.0 cross-compiler and binutils built successfully; buildtools `8375c2dbeaf109c520798cb234d57f0895463201` |
| ARM64 image and QEMU | Current 336 MiB `@minimum-mmc` image built and booted to a visible Tracker/Deskbar desktop in QEMU ARM64, 4 virtual CPUs and 2 GiB RAM |
| NanoKVM | PCIe model, application 2.4.3 and base image v1.4.0; SSH and authenticated API tested |
| Remote controls | HDMI capture, keyboard, reset, full off/on and controller availability through target power-off tested |
| Virtual storage | Raw USB image verified byte-for-byte from ROOBI; selected image survives reset and target power cycle |
| Automated controls | Twenty-two regression checks pass locally, including capture transport and baud transitions; eleven earlier checks also passed GitHub Actions; build, QEMU and real NanoKVM deployment/recovery have been exercised |
| Recovery OS | ROOBI / Debian 11, kernel `5.10.110-33-rockchip`; SSH works independently of virtual media |
| Boot firmware | Board-specific EDK2 v1.1 installed in SPI; native EFI diagnostic completed; current Haiku profile uses mainline DT only; original eMMC boot firmware backed up and cleared |
| Native Haiku on ROCK | EFI bootloader runs, loads the kernel, exits boot services and completes boot-CPU MMU setup; trusted-firmware panic during secondary CPU startup prevents verified kernel/userspace boot |
| Board revision | Owner confirmed ROCK 5 ITX PCB v1.12; current public electrical schematic is v1.11, so exact revision electrical details remain to be checked before raw register work |
| USB recovery | Workstation USB-C connection enumerates as `2207:350b`; remote loader and MaskROM entry, RAM loader download, eMMC/SPI selection and matching read-back hashes verified |
| Serial | NanoKVM UART1 (`/dev/ttyS1`) captures readable DDR/SPL, U-Boot and Linux output at 1,500,000 baud, 8N1; longer input is corrupted and an interactive login has not passed |
| Power LED readback | Reports false while running; use boot IDs, video and actual reachability |

Phase 0 is complete. The final verified image was built from the clean source
revision `288f7e9fab76234747671f881cc4ddf5240e9fa9` (`hrev60097+3`). Subsequent
status-documentation commits do not change that artifact's recorded revision.

| Final validation artifact | Location or result |
| --- | --- |
| Immutable image | `/mnt/HaikuWork/artifacts/images/haiku-arm64-30638bdd666cd13d.img` |
| SHA-256 | `30638bdd666cd13d14a4faf05977d1e7850934949db545bbf0697975469c7cf8` |
| Manifest | Same filename with `.json`; records source, toolchain, packages and EFI loader validation |
| QEMU | `/mnt/HaikuWork/artifacts/qemu/20260911T050646Z-2b567d`; first-login marker passed, no kernel panic, desktop screenshot inspected |
| Hardware | `/mnt/HaikuWork/artifacts/hardware/20260911T050746Z-ae7015`; upload hash matched, nine HDMI frames, recovery returned a different ROOBI boot ID |
| Complete command log | `/mnt/HaikuWork/artifacts/verified-iteration-final.log`; exit status 0 |
| CI | [11 control regression tests passed](https://github.com/jmgasper/haiku/actions/runs/34564745899) |

At the end of phase 0, the target ran ROOBI with `/data/storage-probe.img`
selected, and neither SPI nor onboard OS partitions had been rewritten.
Subsequent recovery and firmware work is recorded below.

After correcting TX/RX wiring on 2026-09-11, NanoKVM `/dev/ttyS1` received a
15,386-byte boot log from DDR initialization through SPL, trusted firmware,
U-Boot and the ROOBI login prompt. The log contained no UTF-8 decoding errors.
Capture ran at 1,500,000 baud, 8N1 without flow control; the other spare port
received no bytes. The ROCK returned over SSH with boot ID
`e9fe9b8a-59ba-480d-8b13-5cf56da85427`. Reset evidence is in
`/mnt/HaikuWork/artifacts/serial/20260911T060852Z-9c3f0e/result.json`.

Carriage returns elicited login prompts, but longer input was corrupted:
`ps` was received as bytes `70 f3` in a paced login attempt. Neither that
attempt nor the unpaced attempt authenticated, and stock NanoKVM picocom also
produced a corrupted input character. Input pacing alone did not solve this.
The observed UART clocks are 25 MHz on NanoKVM and 24 MHz on the ROCK; baud
rounding is a suspected cause, not a measured wire-speed diagnosis. Serial
logging is available, but reliable command entry and the full serial acceptance
gate remain open. All capture sessions were closed, their serial settings
restored, and the target's serial getty reset to a fresh login prompt.
Interactive evidence is in
`/mnt/HaikuWork/artifacts/serial/20260911T061151Z-8446f7/result.json`.

First QEMU desktop capture:
`/mnt/HaikuWork/artifacts/qemu/20260911T045107Z-071bbd/screen.png`.
The serial log records successful framebuffer initialization and first-login
processing. The minimum image lacks some optional libraries: screen-saver and
shortcut input filters and `desklink` report missing dependencies. These do
not prevent this basic desktop boot; broader ARM64 package completeness remains
part of phase 11. No GPU acceleration or networking was tested in QEMU.

First full hardware cycle:
`/mnt/HaikuWork/artifacts/hardware/20260911T045250Z-a193a5/result.json`.
The board booted ROOBI with the Haiku disk attached, and recovery reselected
the known recovery image and returned a new ROOBI boot ID. One HDMI timeout
during reboot was recorded without interrupting recovery. This validates the
deployment/observation/recovery loop, not Haiku boot on the RK3588.

An incremental-build defect left `BOOTAA64.EFI` empty after `fat_shell` tried
to copy a Haiku `BEOS:TYPE` attribute onto FAT. The fork's MMC recipe now skips
attributes for the EFI loader, and the build validates its actual ARM64 PE32+
application header and partition bounds before recording success.

A write-protected USB trial reached userspace but panicked while writing the
BFS journal (`last transaction (2) still open`). QEMU's panic gate prevented
hardware deployment. The lab therefore uses a fresh writable remote copy for
each trial; the immutable local image remains unchanged. Evidence is in
`/mnt/HaikuWork/artifacts/qemu/20260911T050414Z-910fad`.

Inventory observed from this unit: RK3588, 16 GiB RAM, approximately 7.3 GiB
onboard eMMC, 16 MiB SPI loader device, two `10ec:8125` rev 05 Ethernet devices,
ASM1164 `1b21:1164` SATA controller, ES8316 audio and HYM8563 RTC. Linux's running
device tree also identifies RK806/RK8602/RK8603 power devices, FUSB302 Type-C
control and PWM fan. Identification does not establish Haiku driver support.

No NVMe/SATA disk or Wi-Fi/Bluetooth module has been identified as installed.
Additional storage, network peers, audio loopback/receivers, displays and camera
fixtures are needed for the corresponding acceptance tests. The owner confirmed
PCB revision v1.12 on 2026-09-11.

Local evidence and credentials are not published. Initial evaluation lives at
`/mnt/HaikuWork/nanokvm/EVALUATION.md`; raw inventory, screenshots and running
device tree are in its `evidence/` directory. New runs go to
`/mnt/HaikuWork/artifacts`. The original partial snapshots have been supplemented by a full eMMC user-area
and boot-area backup and a new SPI dump. Current backup and restore evidence
is indexed by `state/rock5-backup.json`.

Serial integration validation on 2026-09-11: 17 control/transport checks passed,
including a real PTY byte stream, terminal restoration, a busy UART, a missing
readiness handshake and a dropped capture process. A rebuilt ARM64 image
`184dec46561672f2ebd144645bf5369287716173dd9ceca2f74eea0c8b8ba78c`
passed QEMU first-login review in `artifacts/qemu/20260911T065043Z-d7f19c`.
The hardware cycle in `artifacts/hardware/20260911T065636Z-50ed62` recorded
30,556 serial bytes through deployment and recovery, with no capture errors.
The board ran ROOBI during that trial; this is not a native Haiku pass.

Independent USB reads from MaskROM were verified in
`artifacts/recovery/maskrom-20260911T065323Z-2727e4`. Rockchip tool source
`304f073752fd25c854e1bcf05d8e7f925b1f4e14` was built locally and used with the
Radxa-published `rk3588_spl_loader_v1.15.113.bin` RAM loader (SHA-256
`26baab70e6b915364f7d73d88298366db1bfc346e34683e95d3d11b52492047f`).
Both storage selections reported their expected capacity, and the eMMC first
16 MiB and complete SPI image matched the Linux snapshots. The complete restoration drill passed in
`artifacts/recovery/restore-drill-20260911T070040Z-0383e0`: all 7,818,182,656
eMMC user-area bytes and all 16,777,216 SPI bytes were written through the
MaskROM RAM loader, read back and matched against SHA-256. ROOBI then returned
with boot ID `6eb27d7c-0b28-4062-905c-08c580d9a2ea`. UART capture remained
active throughout. The cleaned full eMMC restoration image has SHA-256
`ce5abe6ec8372a198198d2868031ec5b1feddeff88be85fe5fb15174472286b5`;
the restored original SPI hash is
`fbf27964ee2694dbea2836aa996120a86bffb1a57657a18e7b28e1fbb30009e5`.
See [RECOVERY.md](RECOVERY.md) for the procedure and its physical fallback.

The standalone EFI diagnostic and recovery chainloader build with
`tools/rock5-itx/build-efi-tools.sh`. The diagnostic records GOP geometry,
firmware revision, exception level, configuration tables and the memory map,
and saves available DT/ACPI data to its own USB volume. Its EFI entry and
completion markers passed QEMU; the recovery chainloader also loaded and ran
the diagnostic as a sibling EFI application in
`artifacts/qemu/20260911T070129Z-939dd0`. The native diagnostic subsequently passed on the ROCK in
`artifacts/firmware-trials/20260911T071501Z-93f34c`: EL2, UEFI 2.70,
1920x1080 BGR framebuffer at `0xed3a0000`, 134 memory descriptors, a
184,025-byte mainline DTB, and eight ACPI tables with valid checksums.
The firmware exposed both DT and ACPI. Raw tables and the complete diagnostic
are in that run's `efi-diagnostic/` directory. The matching ROOBI kernel then
booted through the EFI stub using the saved vendor DTB and `acpi=off`;
`/sys/firmware/efi` was present and SSH returned boot ID
`cd809968-0462-4b71-a93a-04f3f9844ddd`. The combined diagnostic/recovery run
showed Linux userspace startup around 93 seconds, including the diagnostic's
30-second pause. Standalone EFI recovery is tested with a 180-second timeout.
F4 subsequently worked after NanoKVM's
BIOS-compatible HID subclass was enabled and the controller restarted.
Ordinary reset and power-button recovery remained in MaskROM; loading the
verified RAM downloader and issuing its normal `rd 0` reset returned ROOBI
through the standalone EFI recovery image. Evidence is in
`artifacts/firmware-trials/maskrom-exit-20260911T074004Z-ce46e1`, with boot ID
`0c0d415f-504d-49ef-bbb1-7fbb879e197b`. This is a working remote firmware recovery
route while EDK2 can initialize USB keyboard input; physical-button entry
remains the fallback for firmware that cannot initialize.

The reusable EFI media packager verified all files by FAT read-back, and its
generated diagnostic image passed QEMU in
`artifacts/qemu/20260911T073935Z-90d8aa`. Capture now supports acknowledged baud
changes. Six real-PTY tests and
sixteen lab-control tests passed, including recovery after a failed baud change.

## First native Haiku trials

The first native Haiku image (`184dec46561672f2...`) stopped in
`dtb_get_interrupt()` with `unsupported interruptCells`. The panic is visible
in `artifacts/hardware/20260911T074159Z-376565/frame-010.jpg`. The running
firmware DTB declares four cells for `arm,gic-v3`; the loader only accepted
three-cell GIC descriptions. The fork now accepts four-cell descriptions with
a zero affinity cell, validates the property length, and continues to reject
PPI affinity partitions that the loader cannot represent.

That first trial also exposed a console problem. EDK2's DW8250 serial library
cannot change attributes, and its Serial I/O wrapper reports that failure as
`EFI_INVALID_PARAMETER`. Haiku discarded the usable interface after requesting
115,200 baud. The fork now keeps firmware serial output for unsupported
attributes and preserves the UART setup; it also avoids programming a baud
divisor without a known input clock. Current hardware capture stays at
1,500,000 baud. This does not resolve the previously observed UART input errors.

Image SHA-256 `be6afb099a08adbfb4c6aa4e7525a21c8f89ebfcbb0da65129fba5280e9a72bd`
passed the QEMU first-login gate in `artifacts/qemu/20260911T074929Z-601c05`.
On the ROCK, `artifacts/hardware/20260911T075114Z-c7daf0` showed Haiku boot icons,
readable loader diagnostics, the selected 8250 UART/GICv3, kernel loading, and
`Calling ExitBootServices. So long, EFI!`. It then reported
`PANIC at PC : 0x000000000005f420` from trusted firmware. Recovery returned
ROOBI boot ID `38fdfe87-d47f-4950-958a-f10633a4a98c`; capture saved 186,551 bytes
without transport errors. The original interrupt-parser failure is resolved
on hardware, but kernel entry has not yet been demonstrated.

The panic address was checked against the exact BL31 segment extracted from
the installed release (segment SHA-256
`9a365c355a5eec4eebc863dc62afbc3b2ac6ca46d7e9f141463c743767acf314`). It is in
the lower-exception-level entry path after a pending SError check. The access
that caused that error remains unidentified. This is separate from Haiku's
earlier parser panic. Further handoff tracing is recorded with subsequent runs.

The final checkpoint image is
`artifacts/images/haiku-arm64-51547c73298843b6.img`, SHA-256
`51547c73298843b609573076315ecd46e483558fc23a8d00762023491c0b2af0`.
It passed QEMU first login in `artifacts/qemu/20260911T080602Z-64cd8a` and was
tested natively in `artifacts/hardware/20260911T081017Z-97bb79`. The image remains
a development build with its original source revision and patch in the manifest;
its functional loader changes are committed in `e1aeff87be`.

The firmware now exposes mainline DT only (`ConfigTableMode=2`,
`FdtCompatMode=2`). This changed Haiku's CPU count from sixteen to the correct
eight. Serial confirms that `ExitBootServices` completes,
`SetVirtualAddressMap` returns `EFI_SUCCESS`, and boot-CPU MMU setup completes.
The same trusted-firmware pending-SError panic then occurs as secondary CPU
startup begins. The loader now keeps serial active during runtime-map setup
and checks the runtime-map return status. The originating access remains open;
single-core bring-up and the CPU startup/PSCI path are the next investigations.
No native Haiku kernel entry, desktop, or peripheral acceptance is claimed.

Automatic recovery returned ROOBI boot ID
`d354f0ef-e6fa-4718-b48e-bc7ba831f1be` with the DT-only profile. The final run
saved 170,769 serial bytes without transport errors. Four native Haiku trials
have exercised the loop; phase 1's twenty-reset gate and complete Linux
functional baseline are still open. UART receive is usable, while the previous
long-input corruption remains unresolved.
