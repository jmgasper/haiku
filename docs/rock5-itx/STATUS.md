# Tested status

Updated 2026-09-12 (Australia/Hobart). This page distinguishes lab readiness from native Haiku
support. No hardware row in the roadmap is accepted merely because Linux or
firmware supports it.

| Area | Evidence and state |
| --- | --- |
| Repository | `jmgasper/haiku`, `rock5-itx` branch; upstream base `855b5d0e3126c86acc84d09f8e859272019bbbc2` |
| Build tools | Haiku GCC 13.3.0 cross-compiler and binutils built successfully; buildtools `8375c2dbeaf109c520798cb234d57f0895463201` |
| ARM64 image and QEMU | Current clean 336 MiB `@minimum-mmc` image passes first login at both EL1 and EL2 with 4 virtual CPUs and 2 GiB RAM; a basic Tracker/Deskbar desktop was inspected during phase 0 |
| NanoKVM | PCIe model, application 2.4.3 and base image v1.4.0; SSH and authenticated API tested |
| Remote controls | HDMI capture, keyboard, reset, full off/on and controller availability through target power-off tested |
| Virtual storage | Raw USB image verified byte-for-byte from ROOBI; selected image survives reset and target power cycle |
| Automated controls | Thirty-two control checks pass locally, including native interrupt decoding, EFI device-path matching, capture transport, baud transitions and shell/file failure paths; build, QEMU and real NanoKVM deployment/recovery have been exercised |
| Recovery OS | ROOBI / Debian 11, kernel `5.10.110-33-rockchip`; SSH works independently of virtual media |
| Boot firmware | Board-specific EDK2 v1.1 installed in SPI; native EFI diagnostic completed; current Haiku profile uses mainline DT only; original eMMC boot firmware backed up and cleared |
| Native Haiku on ROCK | All eight CPUs start; platform EHCI mounts the NanoKVM boot volume; Tracker/Deskbar, NanoKVM keyboard and mouse, RNDIS DHCP and authenticated USB shell work; a short locked 8 GiB memory check passed; sustained acceptance remains open |
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
That checkpoint did not demonstrate native kernel entry, desktop, or peripheral acceptance.

Automatic recovery returned ROOBI boot ID
`d354f0ef-e6fa-4718-b48e-bc7ba831f1be` with the DT-only profile. The final run
saved 170,769 serial bytes without transport errors. Four native Haiku trials
have exercised the loop; phase 1's twenty-reset gate and complete Linux
functional baseline are still open. UART receive is usable, while the previous
long-input corruption remains unresolved.

## Kernel bring-up

Subsequent diagnostic builds used the existing `disable_smp true` setting to
isolate boot-CPU behavior. Native kernel entry is now demonstrated in
`artifacts/hardware/20260911T100833Z-051366`,
`artifacts/hardware/20260911T101639Z-0f6113`, and
`artifacts/hardware/20260911T103227Z-84dea9`. These runs reach the kernel's early
memory allocator, GICv3 setup and architected timer initialization. They stop
with `ESR=0xbe000011` when the first kernel thread enables interrupts. The
SError's reported FAR does not identify the originating access.

The loader's `ISR_EL1` traces show the error is absent at EFI entry and after
CPU, ACPI, device-tree, video and VFS initialization. It is already pending by
boot-volume selection, before loading the kernel, drawing the logo, generating
page tables or exiting boot services. Thus the earlier trusted-firmware panic
at the first secondary-CPU SMC did not establish a fault in PSCI itself.
Further tracing localized the error to scanning unrelated eMMC boot areas.
The original access inside that firmware path is still unidentified.

EL2 QEMU testing exposed a separate timer bug: with VHE enabled, the virtual
timer register aliases no longer signal the EL1 virtual timer interrupt 27.
The kernel now uses the EL2 physical timer and interrupt 26 when running at EL2,
retaining the existing virtual timer at EL1. The firmware DT's `timer` node
confirms these interrupt numbers on the ROCK. Before the change, EL2 QEMU
stalled during xHCI initialization in `artifacts/qemu/20260911T100201Z-24c9e2`.
After it, both EL1 and EL2 first-login gates passed for image
`224bdd5d310c4f811c934a45ed798a300cf1d7aa023a76d8da6c46c5a2b8594a` in
`artifacts/qemu/20260911T101539Z-5a6fda` and
`artifacts/qemu/20260911T101539Z-c69d07`. The iteration script now exercises EL2.
This still uses fixed standard timer PPIs; generalized DT/ACPI timer discovery
remains future work.

The ARM64 EFI runtime map also now supplies identity `VirtualStart` values
for runtime descriptors, consistent with its transition mappings, and maps
runtime MMIO as Device memory. Previously every runtime descriptor supplied
virtual address zero. This is a separate correction: the pending SError is
observed before runtime-map installation.

Each trial preserves its immutable image, source patch, raw UART capture and
automatic recovery result under `/mnt/HaikuWork/artifacts`. Matching unstripped
loader/kernel symbols for recent trials are retained under
`artifacts/symbols/<image-sha256-prefix>/`.

The EFI loader previously scanned every disk in firmware enumeration order.
In `artifacts/hardware/20260911T104615Z-d2ba76`, it scanned the eMMC user area
and boot areas before reaching the USB disk. A pending SError was first seen
on entry to an LBA 1 read of the third raw eMMC device. A separate run without
eMMC protocols had no SError, so an earlier suspected network-probe cause was
ruled out. Network probing did expose independent bugs: absent/invalid load
options could leave an uninitialized buffer, unrelated `key=value` options
could loop forever, and failed probes shut down an interface they never started.
Those cases are now guarded; netboot functionality itself remains unverified.

The loader now uses EFI's loaded-image device handle and device-path ancestry
to try its own disk first, preserving explicit network boot and other disks as
fallbacks. This both respects the selected boot disk and avoids the unrelated
eMMC accesses on normal USB boots. It does not repair firmware access to the
eMMC boot areas; a full fallback/menu scan can still encounter that problem.

Image `d5c8d93f2633007c...` passed four-CPU EL2 QEMU in
`artifacts/qemu/20260911T105407Z-b5e084` and booted natively in
`artifacts/hardware/20260911T105507Z-a86957`. The eMMC protocols were present,
the USB disk matched a 35-byte EFI path prefix, and the pending-error register
stayed clear through handoff. The kernel completed its all-CPU rendezvous,
reported eight logical CPUs, and ran `main2` on CPU 4. The normal SMP default
has been restored. This establishes initial eight-core boot, not the roadmap's
SMP stress, IRQ/IPI routing or memory-integrity acceptance tests.

The same run passed a newly added ACPI guard. DT-only EDK2 still publishes a
small ACPI set containing BGRT, with no DSDT. ACPICA previously dereferenced an
invalid DSDT index and faulted in `AcpiTbLoadNamespace`. The bus manager now
checks for a DSDT before loading the namespace and declines ACPI initialization
when it is absent. ACPICA cleanup still logs `Could not remove SCI handler`;
the kernel continues to device discovery. No native ACPI support is claimed
for this firmware profile.

Boot now reaches `vfs_mount_boot_file_system` and stops with
`did not find any boot partitions!`. The existing USB host drivers bind through
PCI, while the ROCK exposes its host controllers through the device tree.
The Linux reference captured in `artifacts/rock5-usb-baseline-20260911.json`
identifies NanoKVM `3346:1009` at 480 Mb/s on the platform EHCI controller
`fc880000.usb`, bus 2 port 1. The firmware DT declares
`rockchip,rk3588-ehci`, `generic-ehci`, a `0x40000` register window, and SPI 218
(GIC interrupt 250). The first USB target is therefore platform EHCI attachment,
including four-cell GIC decoding, DMA/cache handling and the existing PHY/clock
state. DWC3/xHCI and the other physical ports remain separate work.

Recovery after this run returned ROOBI boot ID
`4fab5591-2479-4103-82ce-29c89b69d815`; UART capture reported no transport errors.
Native userspace and peripheral acceptance remain open.

## Verified kernel checkpoint

A clean build from `c9cf0293450521ca6fa13c8a251e5d3013c2afef`
(`hrev60097+11`) repeated the eight-core native boot after temporary tracing
was removed. The loaded-image disk preference kept the pending SError clear
through handoff, the EL2 physical timer initialized on IRQ 26, and all eight
CPUs completed startup. `main2` ran on CPU 4 and reached the expected
`did not find any boot partitions!` panic. The HDMI debugger frame was inspected.
This is a kernel bring-up checkpoint; it does not establish native userspace,
SMP stress stability, or peripheral acceptance.

| Checkpoint evidence | Location or result |
| --- | --- |
| Immutable image | `artifacts/images/haiku-arm64-6ffb18dd981474d2.img` |
| SHA-256 | `6ffb18dd981474d224183ff6df5946c030f41985becfa69472d569c6f4e02063` |
| Manifest and symbols | Same image basename with `.json`; unstripped loader and kernel in `artifacts/symbols/6ffb18dd981474d2/` |
| Build log | `artifacts/build-20260911T110538Z.log` |
| EL1 QEMU | `artifacts/qemu/20260911T110644Z-ce9634`; four CPUs, first login passed, no kernel panic |
| EL2 QEMU | `artifacts/qemu/20260911T110644Z-b339fb`; four CPUs, first login passed, no kernel panic |
| Native ROCK | `artifacts/hardware/20260911T110842Z-aa232c`; reviewed `result.json`, `trial-serial.log` and `frame-016.jpg` |
| Local checks | 23 regression checks, Bash syntax and whitespace checks passed; `artifacts/control-checks-kernel-checkpoint.log` |
| CI | [23 control regression checks passed for this source revision](https://github.com/jmgasper/haiku/actions/runs/34592601056) |

All evidence paths above are under `/mnt/HaikuWork`. Documentation-only commits
after this checkpoint do not change the image's recorded source revision.
Automatic recovery returned ROOBI boot ID
`db3777db-743f-4192-9b59-1c7c22d139bc`, different from the pre-trial boot ID.
The cycle saved 160,428 serial bytes with no transport errors and restored the
configured EFI recovery image. Platform EHCI support for the NanoKVM USB disk
is the next native boot dependency.

## First native desktop through platform EHCI

Image SHA-256
`5fe52597f02f912f3a731f7f749efa80a0ec74e47c74e9cc562b45468ee30dfe`
booted the native Haiku desktop in
`artifacts/hardware/20260911T113955Z-3c80da`. Both device-tree EHCI controllers
started: `fc800000` on IRQ 247 and `fc880000` on IRQ 250. The second controller
enumerated `NanoKVM USB Mass Storage 0520`; Haiku mounted BFS and packagefs,
ran the first-login script, initialized the framebuffer driver, and displayed
Tracker and Deskbar. `frame-012.jpg` was inspected. All eight CPUs completed
startup, and the trial contained no kernel panic.

The FDT attachment reads register windows and interrupt numbers from the
firmware device tree. It accepts enabled, little-endian generic EHCI devices
with level-sensitive GIC SPIs and identity address mappings. Unsupported
translations, IOMMUs, integrated transaction translators and PPI affinity
partitions are not silently accepted. Kernel FDT decoding now understands
four-cell GIC descriptions with a zero affinity cell and rejects malformed
or out-of-range entries; the separate interrupt-map and extended-interrupt
paths retain their earlier limitations.

The shared EHCI implementation now separates PCI configuration from platform
resources, uses ordering barriers when publishing queues and reading DMA
completion, and guards cleanup after partial initialization. A private ARM64
DMA pool provides Normal Non-cacheable RAM below 4 GiB, including bounce
buffers. Cached allocation contents are cleaned and invalidated before the
mapping changes, and the pool is accessed only through that mapping. This
establishes boot I/O, not sustained integrity or high-memory acceptance.

Only the standard EHCI register interface is used. The board remains on the
previously recorded EDK2 firmware and mainline-DT profile; PHY, clock, reset-line
and power-domain programming is still supplied by firmware. Native resource
management and suspend/resume remain separate work. The implementation was
checked against the [EHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf),
the [generic EHCI binding](https://github.com/torvalds/linux/blob/v6.15/Documentation/devicetree/bindings/usb/generic-ehci.yaml),
and the [GICv3 interrupt binding](https://github.com/torvalds/linux/blob/v6.15/Documentation/devicetree/bindings/interrupt-controller/arm%2Cgic-v3.yaml).

The preceding trial, `artifacts/hardware/20260911T113345Z-4c1fb4`, still stopped
at boot-volume discovery without attempting EHCI initialization. Generic FDT
nodes did not search `busses/usb`; adding that driver-search path allowed the
attachment to run. FDT nodes are registered in source order, so the attachment
does not require the GIC's device-manager node to have been registered before
the already-decodable interrupt specifier is used.

The desktop image passed the four-CPU EL2 QEMU EHCI first-login gate in
`artifacts/qemu/20260911T113829Z-c40999`. Earlier shared-driver checks passed
both EHCI and xHCI first login in `artifacts/qemu/20260911T112954Z-152d78` and
`artifacts/qemu/20260911T112954Z-667655`. Twenty-four host regression checks
passed in `artifacts/control-checks-ehci-platform.log`. These are development
builds, with their source patches in the image manifests; unstripped loader,
kernel and EHCI symbols are saved under each image hash prefix.

Automatic recovery returned ROOBI boot ID
`ec4a0f60-1e30-4b41-8e98-50155a04d299`; capture saved 172,358 bytes with no
transport errors. One USB control request stalled during HID initialization.
The minimum image also reports missing optional screen-saver, game and media
libraries, as it did in QEMU. Keyboard/mouse interaction, USB networking,
sustained storage writes, SMP/memory stress and other peripheral acceptance
remain untested by this desktop observation. NanoKVM's USB network endpoint is
configured as `10.239.6.1/24`, and the image includes `usb_rndis`; loading that
driver alone does not prove a working network connection.

## Recovery cycle audit and interactive diagnostics

The reset/recovery portion of phase 1 now has 20 reviewed cycles with serial
readiness, more than 10,000 captured bytes, no serial transport errors, a Linux
recovery marker, and different pre-trial and recovered SSH boot IDs. The audit
is `artifacts/reset-recovery-audit-20260911.json`; its runs span
`hardware/20260911T065636Z-50ed62` through
`interactive/20260911T122522Z-f213a0`. Eighteen contain the Haiku EFI loader
banner. The first two precede that handoff. Trials used different development
images and firmware profiles, so this validates the recovery mechanism, not
20 consecutive successful Haiku boots. Transfer failures and trials without
serial capture are excluded. UART transmit reliability and a measured Linux
functional baseline remain open.

The clean desktop checkpoint at source `9a1b6b0b2ebc9eaa5eb091483a75a5e36f2ff65c`
has image SHA-256
`bb19f2f074ad705e6910f57c777321734cd436733890979438173d907100c811`.
Its EL2 QEMU EHCI gate passed in `artifacts/qemu/20260911T114903Z-264eac`.
The real desktop was visible in
`artifacts/interactive/20260911T115408Z-597177/frame-007.jpg`, but keyboard and
mouse actions did not visibly change it. The unsupported keyboard control
`0x2710` is the optional `KB_GET_KEYBOARD_ID` query; input initialization proceeds
past it. In QEMU, Ctrl+Alt+Delete opens Team Monitor and its Terminal can run
commands whose output is captured through `/dev/dprintf`. See
`artifacts/qemu-interactive/20260911T121444Z-20d5f4` and
`artifacts/qemu-rndis/20260911T122650Z-7c1a8c`.

Two subsequent native trials stopped before entering the kernel, at the
loader's cache clean-by-set/way instruction (`Loop3`, offset `0x11c0`). These
are `interactive/20260911T120538Z-8264bf` and
`interactive/20260911T121718Z-fbfcbf`; the latter received no keyboard input
during the firmware countdown. Their loader bytes match the preceding clean
desktop image. A temporary exception diagnostic then booted successfully in
`interactive/20260911T122522Z-f213a0`, reporting EL2 with `DAIF=0x3c0` before
cache cleanup. That success does not establish the cause or a fix for the
earlier faults.

The diagnostic desktop trial queued input reads for all three NanoKVM HID
interfaces, but no input transfer completed after direct writes to NanoKVM's
keyboard and relative-mouse character devices. Recovery returned boot ID
`53228bd4-6825-4c57-be8e-ba21e9231e48`; 175,805 serial bytes were captured with
no transport errors. USB input and networking remain under investigation.

RNDIS control setup now matches the legacy CDC ACM `02/02/ff` interface used by
NanoKVM, allocates one notification endpoint packet, and sends a four-byte
packet-filter payload including directed traffic. The earlier fixed eight-byte
buffer rejected QEMU's 16-byte notification endpoint, and its malformed
packet-filter length caused a control stall. The corrected driver obtained
DHCP address `10.0.2.15/24` in
`artifacts/qemu-rndis/20260911T125207Z-4dfbb9`, with two packets transmitted and
received and no interface errors. This test used one EL2 CPU and xHCI for
RNDIS. Its local fixture blocks `usb_ecm` through package settings because
QEMU exposes both RNDIS and ECM configurations; otherwise ECM switches the
configuration under RNDIS. The base image is
`2ae8081dff330fa2464f25ab95c9152196adedf92e70246e4a0dedce0df4b788`, and the
fixture's parent hash and exact settings are recorded in its manifest. Native
DHCP, sustained networking, configuration arbitration and hotplug acceptance
are separate checks.

## Native USB networking and automatic diagnostics

Native RNDIS obtained `10.239.6.146/24` in
`artifacts/interactive/20260911T130941Z-72a087`, using image
`13b806fb0988e0c07341070dba3ff1ce0c1bafda132b6dc02297991488a69932`.
All four pings from NanoKVM's `10.239.6.1` returned, with no packet loss.
An automatic lab startup script reported all eight CPUs, approximately 16 GiB
of managed RAM, the network configuration and USB inventory through
`/dev/dprintf`. Reporting available RAM is not a high-memory integrity test.
The image includes a correction to high-speed interrupt polling intervals:
EHCI now converts the descriptor's microframe exponent to a frame interval and
S-mask instead of issuing bursts of eight polls.

The next trial, `interactive/20260911T131644Z-1a3764`, repeated DHCP and four
successful pings. Its image is
`bf6efe2c9b55d800d95ae80b6812b0a12b3c9b70ce8ca1ad93d6d29b1eaa2674`.
A separate queue-retirement experiment preserved removed periodic links and
waited before reusing their storage. It did not resolve HID inactivity:
direct Ctrl+Alt+Delete reports were accepted by NanoKVM, but no Haiku HID input
transfer completed. Both runs returned to ROOBI with fresh boot IDs and no
serial transport errors. Sustained network traffic and other USB ports remain
separate acceptance work.

Linux input comparison is recorded in
`artifacts/linux-input/20260911T132936Z-8274f1`. ROOBI kernel
`5.10.110-33-rockchip` received the expected Ctrl press/release, relative X/Y
movement and absolute X/Y coordinates on all three NanoKVM interfaces using
the same raw report path. A final metadata SSH call briefly failed; a follow-up
confirmed the same ROOBI boot ID. This validates the physical input path and
these basic Linux events, not the complete Linux hardware baseline.

The cache-handoff diagnostic caught another loader fault in
`interactive/20260911T125610Z-ceced9`: ESR was `0x02000000`, and the saved loader
has an `orr` instruction at the reported PC, offset `0x3100`. This is an
unknown-instruction exception, not evidence of a trapped cache-maintenance
instruction or a valid FAR memory address. Cleaning caches before disabling
the firmware MMU/cache configuration then passed the two native boots above.
Stale instruction contents are a hypothesis; these two successes do not yet
establish a reliable fix. Those development images contained temporary exception
instrumentation, removed in the later checkpoint below.

The login program stored `getopt()`'s integer return value in a `char`.
This ARM64 toolchain defines `__CHAR_UNSIGNED__`, so the `-1` end marker became
255 and login exited through its usage path. Keeping the result as an `int`
allowed an authenticated shell in `artifacts/qemu-shell/20260911T133038Z-625a42`:
an incorrect password was rejected, then the correct private credentials ran
`uname`, `ifconfig` and a service-configuration check. The local image overlay
contains a generated password hash and an explicit opt-in file; telnet binds
only to the USB address. QEMU uses localhost forwarding and blocks the
competing ECM configuration. Native shell access subsequently passed below.

## Native input, authenticated commands and high-memory check

EHCI interrupt queue heads must use a zero NAK reload count (RL), as specified
in section 4.9 of the
[Intel EHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf).
The shared initializer had set RL to three. Correcting it for interrupt queues
and periodic anchors restored native HID polling. The preceding interval and
queue-retirement changes alone had not restored input. The combined fix is
commit `494b6180f6`.

In `artifacts/interactive/20260911T133313Z-6b71f4`, Ctrl+Alt+Delete opened Team
Monitor, an absolute mouse click opened Terminal, and a typed command emitted
`ROCK_NATIVE_INPUT_OK` on UART. Relative mouse movement also visibly moved the
cursor. The private image SHA-256 is
`4e7a2eba33c78d6c241c5e355936f3895749e1af76fd92c453f66c5569e90dde`.
An authenticated shell executed `uname`, `sysinfo`, `ifconfig`, `netstat`, process
inventory and a driver checksum. The listener was bound to `10.239.6.146:23`,
with NanoKVM at `10.239.6.1` as its peer. Workstation access used an SSH tunnel
to NanoKVM, then telnet across the private USB link.

The next trial, `artifacts/interactive/20260911T134405Z-19e940`, repeated the
desktop, keyboard, absolute mouse and authenticated shell checks with all
temporary exception vectors and USB traces removed. Its private image is
`d0e576d822b76616da13c3b405ac475b6cdc3e11f92f236808828369c7764da1`.
This was the fourth successful native boot with the pre-handoff cache clean
(`186268d8cd`), and the first without diagnostic instrumentation. More boot
coverage is required before calling the intermittent loader fault resolved.

A 140,691-byte memory-test executable was uploaded directly to that running
Haiku session, read back through `sha256sum`, and executed only after its hash
matched the workstation binary:
`d5a0e997f1250d61c7bba1954c95460dc465f623c379a148066afe1c28673d0b`.
No new image or reboot was needed for this test. The 8 GiB allocation used
`B_FULL_LOCK`, eight workers and two fill/verify passes, including reads of
another worker's region. It passed with zero mismatches and recorded activity
on all eight CPUs. Since the allocation was resident and exceeded 4 GiB, this
exercises physical RAM above that boundary. This short run does not establish
sustained CPU, thermal, DMA or general memory-management stability.

The packaged checker and its injected-corruption negative control passed in
four-CPU QEMU at `artifacts/qemu-shell/20260911T135151Z-7c2fee`. The 64 MiB check
passed, and a deliberately corrupted word caused the expected failure and
exit status 1. Source is [memory_probe.cpp](../../tools/rock5-itx/memory_probe.cpp).

Normal native `shutdown -r` then stalled with the desktop showing “Asking other
processes to quit.” No new loader banner appeared. ARM64's kernel reset function
was still a stub, but this screen also permits an earlier userspace hang;
the two paths require separate tests. Out-of-band recovery returned ROOBI with
boot ID `dccca9d2-ecae-4cb4-8bfb-dad7fffc6a2c` and no serial transport errors.
The trial's `review.json`, `native-memory.txt`, `probe-upload.txt`,
`software-reboot-command.txt` and `frame-090.jpg` preserve these results.

## Firmware software reset and power-off

Commit `9a23e4317c` replaces ARM64's shutdown stub with PSCI `SYSTEM_RESET` and
`SYSTEM_OFF`. The EFI loader passes the SMC/HVC calling method discovered from
the device tree or ACPI FADT to the kernel. Device-tree discovery accepts PSCI
0.2 and 1.0 compatible lists and validates the method property. The kernel
queries the firmware version before enabling these calls. This follows the
standard interface also used by
[EDK2's PSCI reset library](https://github.com/tianocore/edk2/blob/master/ArmPkg/Library/ArmPsciResetSystemLib/ArmPsciResetSystemLib.c);
it adds no RK3588 register or clock programming.

Four-CPU QEMU completed quick reboot, a fresh authenticated login and power-off
at both EL2/SMC (`artifacts/qemu-shell/20260911T141410Z-d1a82d`) and EL1/HVC
(`20260911T141639Z-d5aa80`). Normal desktop reboot also completed in
`20260911T142204Z-cdbcf3`, followed by fresh login and quick power-off. An earlier
normal-path attempt (`20260911T141909Z-0ef1e6`) never reached a login prompt and
therefore did not test shutdown.

Native trial `artifacts/interactive/20260911T141640Z-a9ec13` used private image
`bf5752c86a8122d8d32fe9bcfad77729dc3e7364e49e4ee57dccea70d7453fe4`.
The ROCK reported PSCI 1.1 via SMC. `shutdown -rq` issued a firmware reset,
returned through the EFI loader to Haiku, restored DHCP and accepted a new
authenticated shell. A 64 MiB, eight-worker, two-pass memory check then passed.
This is an actual software reboot without a NanoKVM reset pulse between boots.

Native `shutdown -q` reached `SYSTEM_OFF` without returning; serial output
stopped and a requested HDMI capture timed out. The session consequently recorded
an error for that capture. Reset alone did not bring the powered-off board back;
the existing recovery power-button fallback returned ROOBI with boot ID
`b207da33-2a4a-4abd-b0a4-b9e691751fb1`. NanoKVM remained reachable. These are
observations of firmware power-off and recovery, not electrical power measurement
or front-panel/suspend qualification. Native normal reboot passed in the later binary-transfer trial below; normal
power-off remains untested on the ROCK.

Repeated QEMU shell testing exposed an intermittent login failure, both on an
initial boot and after software reboot. In `20260911T143736Z-6d3a56`, keyboard
diagnostics showed DHCP, an active RNDIS interface and a TCP listener on the
correct USB address, but remote login did not open. Thus a printed shell
configuration marker is not sufficient acceptance. Investigation and repeated
authenticated checks remain necessary before calling this lab path reliable.


## Service listener and binary transfer checks

The intermittent missing login prompt was reproduced natively in
`artifacts/interactive/20260911T152843Z-e77e29`. A temporary server trace showed
`select min=4 max=4 pipe=4`: `pipe()` had returned read descriptor 4 and write
descriptor 3. The initial `select()` range was calculated from the write end,
so it excluded the read end. The listener never received the update wakeup when
the USB login socket was added. The authenticated attempt timed out, and
recovery returned ROOBI boot ID `0a7a9217-800c-408f-9239-914dad9a74ec`.

The server now derives its initial range from the read descriptor. Temporary
traces have been removed. `rock5_services_probe` links the actual server listener
and forces its read descriptor to 64 while the write descriptor remains lower.
It waits for the empty listener to block, adds a loopback service, and requires
an actual child process reply. Its `--legacy-range` negative control deliberately
restores the old range and must fail to receive that reply.

All combined QEMU checks passed in `artifacts/qemu-shell/20260911T154454Z-708873`,
using private image SHA-256
`f04c260283dd5ad01af2431caba3789aae9e3c6f5c636b55eee491368e2267ab`:
incorrect-password rejection, authenticated commands, the service regression and
negative control, locked memory checking with injected-corruption detection,
CPU affinity and cross-core clock ordering, 32 fork/exec/COW checks, eight
protected-page faults, an 8 MiB binary round trip, rejection of truncated input,
and normal desktop reboot, fresh login and power-off through PSCI/SMC.
The clock/fork/fault tests are short integrity probes, not sustained qualification.

Large base64 uploads through an interactive terminal exceeded the five-minute
limit even after fixing simultaneous send/receive. A subsequent 4 KiB test in
`artifacts/interactive/20260911T151858Z-d617de` confirmed that bootstrap Bash
also lacks `/dev/tcp`. The lab image now packages `rock5_file_transfer`, a bounded
native binary stream client. Host tools relay files over NanoKVM SSH and its
private USB network, check lengths and SHA-256, and retain partial results as
unaccepted files on error. No extra NanoKVM file copy is needed. The reusable
session client supports commands, uploads and downloads while keeping UART,
HDMI and automatic recovery active. All 32 host control checks passed in
`artifacts/control-checks-20260911-binary-files.log`.

A separate QEMU run, `artifacts/qemu-shell/20260911T153111Z-027922`, had no DHCP
address and zero USB network packets. That run did not reach shell configuration;
it is a separate startup failure from the traced listener bug. RNDIS control
waits and initialization remain under investigation. QEMU packet captures and
private shell images contain authentication material and remain local.


Native validation of that image is in
`artifacts/interactive/20260911T154757Z-47e4ab`. The service regression and its
negative control passed on the ROCK. All eight pinned workers sampled the
clock without backwards readings or wrong-CPU observations; 32 fork/exec/COW
checks and eight protected-page faults passed. A locked 8 GiB allocation passed
two checks with eight workers in 2.97 seconds. This is a short integrity result.

The native 8 MiB binary upload completed in 18.97 seconds and its first download
in 16.18 seconds, including authentication and checksum verification. The
returned file matched the source byte for byte, with SHA-256
`13d3d2a1d7eee5c4ff92996af3a186741320b02dd95451eaff323772fb966d95`.
Normal `shutdown -r` then reached PSCI reset, booted Haiku again without a
NanoKVM reset pulse, and accepted fresh authenticated commands. The stored file's
hash, another platform probe, and a 64 MiB memory check passed after reboot.

During a subsequent download, the NanoKVM itself stopped answering SSH, HTTP,
ping and ARP at `192.168.1.8`; the workstation's Ethernet link and router
remained reachable. The controller outage's cause is unknown. The 425,984-byte
partial download was not accepted, and queued executable-deployment and normal
power-off tests were not run. Automatic recovery could not contact NanoKVM, so
the session is explicitly `recovery_failed`; the last verified target state is
Haiku running after normal reboot. Twelve native boots have now reached the
startup probe with the cache pre-clean change; this is not a long-run stability
acceptance. Controller access must be restored before another hardware trial.

Shell and file SSH connections now request a five-second keepalive interval
with three missed replies allowed; the workstation's inherited 300-second
interval had delayed failure detection. Interactive sessions also return a
nonzero process status when the trial or recovery fails. All 32 host checks
passed after these changes in
`artifacts/control-checks-20260911-final-shell.log`.

## Bounded RNDIS control waits

Topic branch `rock5-rndis-control`, source commit `7d51890c72`, returns failed
initialization sends immediately and limits response-notification waits to five
seconds. Failed opens cancel the notification transfer, and subsequent opens
clear old semaphore counts. A timeout or interruption during an optional query
also stops initialization, since a late response must not be consumed as the
next command's reply. This bounds the notification wait; it does not establish
the cause of the earlier intermittent startup failures.

Three temporary fault injections were exercised in QEMU:

| Injected failure | Observed result | Evidence under `artifacts/rndis-faults/` |
| --- | --- | --- |
| Drop control-response notifications | Initialization returned a timeout after approximately five seconds; the desktop startup probe completed | `20260911T162522Z-a711a9` |
| Fail the initialization send | Open returned the send error without entering the response wait | `20260911T162858Z-8abd58` |
| Return a timeout from the maximum-frame-size query | Initialization stopped before media-state and link-speed queries; no shell was configured through the failed adapter | `20260911T163744Z-acad91` |

An earlier query-fault attempt, `20260911T163304Z-7c63e9`, stalled during general
userspace startup before reaching either the RNDIS driver or startup probe. It
is retained as a failed trial, not counted as a fault-handling pass. The later
attempt used the same immutable image and reached the intended injection.

All injections were removed before the final development image was built.
The full QEMU suite passed in `artifacts/qemu-shell/20260911T163918Z-9a7a33`:
authenticated commands, memory and platform probes, service-listener regression
and negative control, an 8 MiB binary round trip with truncated-input rejection,
normal reboot, fresh login and normal power-off. All 32 host control checks
passed in `artifacts/control-checks-rndis-timeouts.log`.

A clean build from `7d51890c72d5f22986b95b53e8138ccb2d9e29af` has base-image
SHA-256 `9ab5f631c0ae3a78dc027142b90df7530554cc796f11897521d99f01c10bc238`.
Its private shell overlay also passed the full suite in
`artifacts/qemu-shell/20260911T164148Z-612466`, including normal reset and off.
The manifest and gate are indexed locally by
`state/rock5-rndis-control-checkpoint.json`; the private overlay remains local.

Native validation of this change remains pending: NanoKVM is still unreachable
at its saved address, and its saved mDNS name did not resolve. The hardware
checkpoint on `rock5-itx` remains available separately. Response-length and
request-ID validation, retry behavior and sustained USB network testing remain
open; the new timeout is not evidence of full RNDIS reliability.
