# Tested status

Updated 2026-09-11. This page distinguishes lab readiness from native Haiku
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
| Automated controls | Twenty-four regression checks pass locally, including native interrupt decoding, EFI device-path matching, capture transport and baud transitions; build, QEMU and real NanoKVM deployment/recovery have been exercised |
| Recovery OS | ROOBI / Debian 11, kernel `5.10.110-33-rockchip`; SSH works independently of virtual media |
| Boot firmware | Board-specific EDK2 v1.1 installed in SPI; native EFI diagnostic completed; current Haiku profile uses mainline DT only; original eMMC boot firmware backed up and cleared |
| Native Haiku on ROCK | All eight CPUs start; platform EHCI mounts the NanoKVM boot volume; native userspace and a basic framebuffer Tracker/Deskbar desktop have booted; HID interaction and stress acceptance remain open |
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
