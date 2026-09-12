# Tested status

Updated 2026-09-12 (Australia/Hobart). This page distinguishes lab readiness from native Haiku
support. No hardware row in the roadmap is accepted merely because Linux or
firmware supports it.

| Area | Evidence and state |
| --- | --- |
| Repository | `jmgasper/haiku`, `rock5-itx` branch; upstream base `855b5d0e3126c86acc84d09f8e859272019bbbc2` |
| Build tools | Haiku GCC 13.3.0 cross-compiler and binutils built successfully; buildtools `8375c2dbeaf109c520798cb234d57f0895463201` |
| ARM64 image and QEMU | Current clean 336 MiB `@minimum-mmc` image passes first login at both EL1 and EL2 with 4 virtual CPUs and 2 GiB RAM; a basic Tracker/Deskbar desktop was inspected during phase 0 |
| NanoKVM | PCIe model, application 2.4.3 and base image v1.4.0; staged downloads passed before/after native reboot, but simultaneous USB/Ethernet relay still causes outages; the latest outage did not recover through the hardware watchdog |
| Remote controls | HDMI capture, keyboard, reset, full off/on and controller availability through target power-off tested |
| Virtual storage | Raw USB image verified byte-for-byte from ROOBI; selected image survives reset and target power cycle |
| Automated controls | Fifty-six host checks pass locally, including explicit SSD-session USB reset interlocks, bounded concurrent storage writes and corruption detection, NVMe trim interval boundaries, the firmware PCIe profile, NVMe sector guards, ARM64 cache-line decoding, RNDIS packet bounds, native interrupt decoding, EFI device-path matching, capture transport, baud transitions and staged file verification/failure paths; build, QEMU and real NanoKVM deployment/recovery have been exercised |
| Recovery OS | ROOBI / Debian 11, kernel `5.10.110-33-rockchip`; SSH works independently of virtual media |
| Boot firmware | Board-specific EDK2 v1.1 installed in SPI; native EFI diagnostic completed; current Haiku profile uses mainline DT only; original eMMC boot firmware backed up and cleared |
| Native Haiku on ROCK | All eight CPUs start; Tracker/Deskbar, NanoKVM input, RNDIS DHCP and authenticated USB shell work; a short locked 8 GiB memory check passed; Samsung NVMe bounded raw I/O has independent Linux hashes; the 238 GiB SSD installation boots repeatedly and has passed large-file persistence after normal reboot and shutdown/startup; the installed hrev60097+57 update also passes filesystem TRIM and reboot readback; intermittent USB control failures and sustained acceptance remain open |
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

The installed 256 GB Samsung 950 Pro NVMe SSD is identified in ROOBI. The owner
authorizes erasing its existing data for testing and eventual Haiku installation.
Native Haiku PCIe/NVMe discovery and a full-capacity SSD installation now work
through the explicit firmware profile described below. No SATA disk or
Wi-Fi/Bluetooth module has been identified as installed.
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

## Native RNDIS retest and recurring controller outage

NanoKVM returned after a controller reboot at approximately 21:25 UTC on
2026-09-11. Its boot ID was `e74dfc19-8368-4fda-9a20-eba86b42b8b1`.
The expired web session was renewed, and recovery with active UART capture
returned ROOBI boot ID `c1d0ee95-6fb6-408e-ad26-2ce2ea6a9bc7`.
Evidence is in `artifacts/controller-recovery/20260911T212742Z-51072a`.

The pre-recovery screenshot showed the old Haiku session in the kernel debugger
with `last transaction (6) still open!`. Its stack ran from a syslog write through
BFS to `cache_start_transaction()`. The capture gap prevents determining whether
this happened during the original controller outage or the later controller
restart. It does not establish the cause of the controller's LAN failure.

The clean RNDIS candidate was then deployed as a fresh USB image in
`artifacts/interactive/20260911T212914Z-78e307`, private image SHA-256
`b4bd947372f2189b5cda5c7c5d807842e3d2b5c404a11ea639abdb15c2e8ef28`.
All eight CPUs started and the adapter obtained `10.239.6.102`. NanoKVM's gadget
MAC changed across its reboot, so each trial must use its observed DHCP address.
Authenticated commands, the service-listener regression and negative control,
the ten-second platform probe, and a locked 64 MiB/eight-worker memory check
passed. This is the thirteenth native startup observed with the cache pre-clean
change across the recorded development trials, not sustained qualification.

The 8 MiB upload passed with the expected checksum in 19.35 seconds. During the
following download, NanoKVM again stopped answering LAN SSH and ARP; UART capture
and the independent controller-health SSH stream also disconnected. The router
remained reachable. The 65,536-byte incoming file is retained but unaccepted.
Live executable upload/execution, reboot persistence and normal power-off were
not reached. The session is `recovery_failed`, not a full native gate pass.

Controller samples in `artifacts/controller-health/20260911T212845Z-534dae`
ended at 21:34:08 UTC. Across 64 samples, available memory stayed above
50,912 KiB and the highest reported temperature was 44.095 degrees Celsius.
The last sample had 60,736 KiB available. These five-second snapshots do not
exclude a sudden failure between samples or identify its cause.

Before another Haiku download trial, compare NanoKVM-to-workstation SSH traffic,
ROOBI-to-NanoKVM USB traffic, and the combined relay separately while ROOBI runs
from eMMC. A local comparison script is prepared but has only passed Python
syntax/CLI checks; its hardware cases have not run. Sipeed documents an optional
[controller watchdog](https://wiki.sipeed.com/hardware/en/kvm/NanoKVM/user_guide.html).
Its installed behavior and recovery operation still need verification. The
pending work is indexed locally by `state/controller-outage-next.json`.

Inspection of the vendor's 2.4.3 source clarifies that this documented watchdog
is a userspace service monitor: `kvm_system` checks a heartbeat file each second
and calls `system("reboot")` after more than ten missing checks. It does not arm
a hardware watchdog in that loop. Thus enabling the documented flag alone
cannot establish recovery from a whole-kernel hang; a LAN-only failure may also
leave the monitored service alive. See the pinned
[watchdog loop](https://github.com/sipeed/NanoKVM/blob/3b2ba7c0c1214f44da9d328f90bbdd025fac0413/support/sg2002/kvm_system/main/src/main.cpp#L238-L256)
and [heartbeat check](https://github.com/sipeed/NanoKVM/blob/3b2ba7c0c1214f44da9d328f90bbdd025fac0413/support/sg2002/kvm_system/main/lib/system_state/system_state.cpp#L506-L525).
The installed `/dev/watchdog*` devices, driver identity, timeout and clean
disarm behavior must be inspected on the controller before planning a hardware
watchdog trial. No watchdog setting has been changed.

## Controller watchdog and transfer isolation

Subsequent inspection found an installed `soph_wdt` hardware driver exposing
`/dev/watchdog0`, with `nowayout=N` and a device-tree reset connection. The
installed module's start, timeout and stop disassembly matches the relevant
paths in the pinned [vendor implementation](https://github.com/sipeed/LicheeRV-Nano-Build/blob/d4003f15b35d43ad4842f427050ab2bba0114fa5/osdrv/interdrv/v2/wdt/wdt.c).
Module SHA-256 is
`05d687320c7a7872281b230b0528ab42d15bd9065828ac3a1730a9584c4a4f2d`.
A requested 30-second timeout reports 21 seconds through `GETTIMEOUT`, while
`GETTIMELEFT` immediately after feeding reports 30. Tests use frequent
acknowledged heartbeats and do not assume the reported timeout is exact.

| Watchdog check | Evidence under `artifacts/controller-watchdog/` |
| --- | --- |
| Enable, feed, disable and remain alive for 35 seconds afterward | `20260911T215941Z-d9eb06` |
| Deliberately stop feeding; NanoKVM returns with a new boot ID in 50.6 seconds, ROOBI's boot ID unchanged | `20260911T220112Z-91d9cb` |
| Acknowledged host heartbeats keep it alive for 40 seconds, followed by clean disarm | `20260911T220318Z-c2d2f0` |
| Stop host heartbeats; NanoKVM returns in 68.3 seconds without a software reboot command | `20260911T220551Z-c7462a` |

The temporary guard is scoped to individual tests; no watchdog startup service
was installed. It leaves the hardware timer armed if the SSH heartbeat fails.
An alternate SSH route through ROOBI's LAN connection to NanoKVM's USB address
was also verified while ROOBI ran from eMMC.

Three 8 MiB Linux comparisons passed with matching checksums and unchanged
controller/ROOBI boot IDs: NanoKVM-to-workstation SSH in 3.30 seconds,
ROOBI-to-NanoKVM USB with local hashing in 4.50 seconds, and the combined relay
in 5.04 seconds. Evidence is in `artifacts/controller-baseline/` directories
`20260911T220423Z-67d337`, `20260911T220441Z-cc2f73`, and
`20260911T220510Z-46eed5`. SSH compression was disabled.

Native session `artifacts/interactive/20260911T220734Z-efae81` passed the
service/platform/memory checks, a 141,227-byte executable upload followed by
execution, the 8 MiB upload, and an 8 MiB USB-only download hashed on NanoKVM.
The combined USB/SSH relay then reproduced the controller outage, leaving an
unaccepted 1,507,328-byte partial file. Normal reboot and power-off were not run.
The hardware watchdog restored NanoKVM with a new boot ID. Initial automatic
ROOBI recovery failed because SSH became available before the web API; the
local wrapper now waits for API readiness. Recovery subsequently passed with
125,417 captured UART bytes in
`artifacts/controller-recovery/20260911T221542Z-73a13b`.

This narrows the failing workload, without identifying the underlying defect.
No kernel panic message reached the NanoKVM kernel-log capture before its SSH
stream ended, and the guard's failure diagnostic was absent after reboot.
Available memory stayed above 53,792 KiB in 67 five-second samples; their maximum
reported temperature was 42.347 degrees Celsius. These observations do not rule
out a sudden failure between samples.

The lab's default download transport now receives and flushes a unique NanoKVM
scratch file before copying it to the workstation. It verifies the receipt's
length and SHA-256, then the guest's before/after checksum, and removes the
scratch file on success. The original relay remains an explicit diagnostic
option. All 35 host checks pass, including real local receiver processes for
successful, truncated and corrupted staged transfers. A staged 8 MiB ROOBI
comparison passed in 13.95 seconds and removed its scratch file:
`artifacts/controller-baseline/20260911T222015Z-8a64b1`.

In native session `artifacts/interactive/20260911T222106Z-571446`, the service,
platform and memory checks passed again. The 8 MiB upload took 18.24 seconds and
the first staged download passed in 22.34 seconds, with its scratch file removed.
A short locked 8 GiB memory test passed before normal PSCI reboot. The new boot
accepted authenticated commands and verified the persisted file's checksum.
The second staged download then caused another controller outage during its
USB-receive stage, before copying to the workstation. Thus simultaneous bulk
Ethernet traffic is not required, and staging alone does not solve the problem.
Normal power-off was not reached.

This time the watchdog and API-readiness wait completed recovery automatically:
NanoKVM returned with a new boot ID, a fresh UART capture recorded 125,586 bytes,
and ROOBI became reachable. Evidence is in
`artifacts/controller-guarded-native/20260911T222106Z-5b6ccb/controller-recovery.json`.
The native test remains an error despite successful recovery. The verified
guard and wrapper are now available as `controller_watchdog.py` and
`guarded_session.py` in the lab tools.

Staged reception now also limits its TCP receive window and paces data at
256 KiB/second. Its 35 host checks pass, and an 8 MiB Linux comparison passed
in 45.41 seconds in `artifacts/controller-baseline/20260911T224323Z-05a9b3`.
An earlier paced attempt, `20260911T224132Z-15138a`, failed its ROOBI SSH
prerequisite before transferring data and is retained separately. The paced
transport has not yet passed native qualification; no controller driver fix
is claimed.

## RNDIS Ethernet frame bounds

Source revision `3b9f2720e554f601ef81239643a7f2c5dba16f9e` corrects the size
reported through `ETHER_GETFRAMESIZE`: RNDIS's
[maximum-frame-size query](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/oid-gen-maximum-frame-size)
excludes the Ethernet header, while Haiku's interface expects that header to be
included. The driver also validates each received message and destination
capacity before copying, using byte-wise little-endian decoding for unaligned
buffers. Invalid or incomplete messages discard the remaining batch without
copying data. A missing or unusable maximum frame size now fails device open.

All 36 host checks passed, including the actual packet extraction code under
AddressSanitizer and UndefinedBehaviorSanitizer: full 1514-byte frames,
insufficient destination capacity, truncated messages, length/offset limits,
unaligned buffers and padded packet batches. Evidence is
`artifacts/control-checks-rndis-framing.log`. The clean ARM64 build produced
base image SHA-256
`7434a37c28d8ec4644f44a06a9eed284ebefad4ddcf900aee457d3ce8d5e4cf5`.
Its private lab image has SHA-256
`6fcb9cbb51a2258bc029159403425490074686d3c2fa88782724e346c49ae23c`.
The full QEMU suite passed in `artifacts/qemu-shell/20260911T230644Z-a4d84e`,
including the 8 MiB round trip, normal reboot and normal shutdown.

The first native trial, `artifacts/interactive/20260911T230930Z-43f108`,
booted and reported MTU 1500 instead of the previous 1486. Its command script
then failed with status 127 because the minimal image lacks `grep`; subsequent
checks did not run. Automatic ROOBI recovery succeeded and the controller
watchdog disarmed normally. The retry uses a shell built-in for the MTU check.
The NanoKVM transfer outage remains a separate unresolved issue.

The retry in `artifacts/interactive/20260911T231505Z-d3c156` passed the MTU,
service, eight-CPU platform and 64 MiB memory checks. Normal shutdown reached
PSCI SYSTEM_OFF, the USB link went down, and the remote power button started a
fresh Haiku boot that passed authenticated checks. NanoKVM kept the same boot
ID through that cycle. Its screenshot retained the previous desktop image;
the screenshot alone does not demonstrate HDMI signal loss. Power evidence is
recorded in the run's `normal-power-off.json`.

An 8 MiB upload passed in 28.60 seconds. A staged download paced at 256 KiB/s
passed in 54.03 seconds. After a short locked 8 GiB memory check and normal
PSCI reboot, the persisted file checksum, MTU and platform/memory checks passed
again. A second paced download passed in 53.79 seconds, and an unpaced staged
download passed in 22.11 seconds. All returned files matched SHA-256
`13d3d2a1d7eee5c4ff92996af3a186741320b02dd95451eaff323772fb966d95`.
These are bounded successful tests, not sustained transfer qualification.

The original simultaneous relay then reproduced the controller outage,
leaving an unaccepted 1,638,400-byte partial file. The watchdog heartbeat ended,
and this time NanoKVM did not return with a new boot ID within the 150-second
recovery gate. ROOBI was also unreachable and no Rockchip USB recovery device
was visible. The session ended `recovery_failed`; a physical NanoKVM power cycle
was requested. Evidence is in
`artifacts/controller-guarded-native/20260911T231505Z-09a084/controller-recovery.json`.
The watchdog's earlier successes do not establish recovery from every outage,
and the RNDIS bounds correction does not fix this relay failure.

## ARM64 instruction-cache synchronization

Source revision `e10bf161c52aa09d566216f3ab1be2bbe3a1f844` corrects a shift
expression in `arch_cpu_sync_icache()`. Instruction-cache line size must use
the low four bits of `CTR_EL0`; the old expression shifted the register value
instead of masking it, producing an invalid C++ shift count. The change uses
small shared decoding helpers and adds compiler memory clobbers to the existing
cache-maintenance barriers. The architectural sequence remains data clean,
barrier, instruction invalidate, barrier and instruction synchronization, as
described in [Arm's cache-maintenance explanation](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/caches-self-modifying-code-implementing-clear-cache).

All 37 host checks passed in `artifacts/control-checks-arm64-cache.log`. The
cache test compiles the actual decoding helpers with UndefinedBehaviorSanitizer
and checks explicit register values with different instruction/data line sizes.
Substituting the old expression into a temporary copy makes that same test fail
with an excessive-shift diagnostic; evidence is in
`artifacts/arm64-cache-regression/`. The source helpers remain corrected.

The clean build has base-image SHA-256
`54ca9b7cf76eed624e6d47e753c903ac002dae3558461d082c5b1dde4b42d01a`
and private-image SHA-256
`8c32202ee225e6aafc36238406b537ee847af8f01c5295be5f60691a49751327`.
The full QEMU suite passed in `artifacts/qemu-shell/20260911T234507Z-90b6ce`,
including 8,192 instruction-replacement checks across four CPUs, the memory,
platform and service probes, the 8 MiB transfer and negative controls, normal
reboot, fresh login and normal shutdown.

The new `rock5_cache_probe` writes functions crossing 64-byte boundaries and
one page boundary, calls `clear_caches()`, and verifies their changed return
values after pinning the thread to each CPU. Each executing CPU performs its
own instruction synchronization; code is never modified concurrently with its
execution. See [Arm's discussion of cross-CPU instruction synchronization](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/caches-self-modifying-code-working-with-threads).
QEMU does not prove physical cache coherence. Native validation remains pending:
the owner reported power-cycling NanoKVM, but its saved LAN address remained
unreachable and its saved mDNS name did not resolve at the subsequent check.
No new controller boot ID or native test result has yet been observed.

## Controller recovery and installed NVMe fixture

NanoKVM subsequently returned at its saved LAN address with boot ID
`84b69ae8-4d9e-4c03-9f5c-6c9547084819`. Recovery with UART capture passed in
`artifacts/controller-recovery/20260911T235338Z-d1dc1a`, recording 130,051 bytes
without capture errors and ROOBI boot ID
`907709b6-075c-481e-9863-b42af8a54ef6`. The initial screenshot showed the previous
Haiku session in `last transaction (7) still open!` during a syslog/BFS write.
The capture gap does not establish whether that panic preceded or followed
the controller outage/restart, or explain the controller failure.

Read-only Linux inventory in `artifacts/nvme-inventory/20260911T235503Z-f4fa6c`
identifies `Samsung SSD 950 PRO 256GB`, firmware `1B0QBXX7`, at PCI address
`0000:01:00.0` (`144d:a802`). The namespace is 256,060,514,304 bytes with
512-byte logical/physical sectors. Linux reports an 8.0 GT/s, two-lane link;
the SSD advertises a four-lane maximum. This records the observed configuration,
not a diagnosis of lane sharing or a throughput measurement.

The SSD has an unmounted 1 GiB FAT partition and a 254,984,323,072-byte Btrfs
partition. Neither was mounted or modified by the inventory. ROOBI continues
to run from eMMC with kernel `5.10.110-33-rockchip`; Haiku support and SSD
functional acceptance remain open. ROOBI's clock was approximately 37 minutes
behind the workstation during inventory, so artifact names use workstation UTC.

After inventory, the owner explicitly authorized discarding all existing data
on this SSD, using it for testing, and eventually installing Haiku on it. This
drive is now an available scratch fixture; that authorization does not establish
native driver support or a completed installation.

## Native cache validation and SSD reference patterns

The cache-fix image subsequently passed a bounded native trial in
`artifacts/interactive/20260911T235552Z-f9a804`. Before and after normal reboot,
all eight CPUs passed 32-round and 256-round cache probes: 16,384 and 131,072
instruction checks per boot, or 294,912 checks with zero mismatches overall.
The service regression, ten-second platform probe and 64 MiB memory check
passed on both boots. A locked 8 GiB/eight-worker/two-pass memory check passed
in 3.52 seconds before the reboot. UART recorded the normal reset request and
two independently started authenticated Haiku sessions. The inspected initial
screenshot shows Tracker and Deskbar.

ROOBI recovery passed with boot ID `79c7c397-b254-49a8-8fcf-045c2b94fa52`.
UART capture saved 232,353 bytes without errors, and the scoped controller
watchdog disarmed normally. All 144 five-second controller samples retained
the same NanoKVM boot ID; minimum available memory was 46,756 KiB and maximum
reported temperature was 44.095 degrees Celsius. Evidence is in
`artifacts/controller-health/20260911T235545Z-e59823`. The collector was stopped
manually after recovery; its interrupted SSH exit is not an outage. This trial
did not exercise bulk downloads or establish sustained cache/memory acceptance.

With the owner's erasure authorization, the physical Samsung SSD then passed
a bounded Linux direct-I/O test in
`artifacts/nvme-linux-baseline/20260912T000649Z-0d883f`. The test checked the
model, serial, firmware, size and absence of mounted partitions/holders before
opening the exact device exclusively. It wrote and flushed two 8 MiB patterns
at offsets 2 GiB and 5 GiB, then read them through `O_DIRECT`; both SHA-256
values matched. The patterns and offsets remain recorded for future native
Haiku read comparisons. Existing filesystem data is disposable; this test
overwrote 16 MiB but did not alter the partition table. These small transfers
do not establish sustained throughput or power-loss durability.

## Initial ARM64 NVMe emulation checks

An exploratory [QEMU NVMe device](https://www.qemu.org/docs/master/system/devices/nvme.html)
with a separate 8 GiB sparse namespace exposes the existing ARM64 driver as
`/dev/disk/nvme/0/raw`. This exercises generic PCI/NVMe independently of the
RK3588 PCIe host, which remains unimplemented. The local test seeds distinct
8 MiB patterns at offsets zero and 4 GiB; all virtual disks and evidence remain
under `/mnt/HaikuWork`.

The first run, `artifacts/qemu-nvme/20260912T000322Z-e48924`, read both initial
patterns and completed a checked 8 MiB write with `fsync`, then stalled during
another readback into an existing BFS file. Host inspection after QEMU stopped
confirmed the written region's expected checksum. The complete test failed its
180-second command deadline; it did not reach reboot or shutdown validation.
Its process listing retained a running `dd` command.

A second run, `artifacts/qemu-nvme/20260912T001016Z-e5a4bf`, streamed read data
directly into the checksum program and stalled on the first bulk read, before
any test write. This shows that rewriting an existing BFS destination file is
not required for the stall. Both runs retain their unchanged source image,
command script, serial log and failed result. Device enumeration and individual
successful transfers do not establish NVMe readiness; the blocked thread's
kernel stack was the next diagnostic target.

## ARM64 NVMe interrupt starvation and polling fallback

Two further diagnostic runs localized the stall. In
`artifacts/qemu-nvme/20260912T002241Z-b3286d`, the writing thread was running on
CPU 0, stopped immediately after `ConditionVariableEntry::Wait` re-enabled
interrupts. Its caller was NVMe `await_status`. Switching KDL to the actual CPU
was necessary: ARM64's current `bt` command ignores its advertised thread-ID
argument. Matching kernel/driver symbols and all CPU stacks are preserved.

The driver used legacy INTx without acknowledging the completion queue in its
interrupt handler. The waiting thread normally consumes that queue, but a
reasserted level interrupt can prevent it from returning to the polling code.
The [QEMU 8.2.2 NVMe model](https://github.com/qemu/qemu/blob/v8.2.2/hw/nvme/ctrl.c)
asserts the pin for pending completions and deasserts it when they are consumed.
This supports interrupt starvation as the cause, rather than a locked condition
variable or a requirement to overwrite a BFS destination file.

The ARM64 candidate keeps PCI INTx disabled and uses polling when MSI/MSI-X
cannot be configured. It installs no handler for that polling path, records
the actual installed interrupt vector for teardown, and caps polling backoff
without an unbounded shift. Successful MSI/MSI-X setup retains interrupt mode.
Polling is a compatibility fallback with a performance cost; controller-stall
recovery and native MSI routing remain separate work.

The candidate passed the entire original reproduction in
`artifacts/qemu-nvme/20260912T003443Z-c825f9`: both initial reads, an 8 MiB write
with `fsync`, readback before and after normal reboot, normal power-off and
independent host checksums. Its private image SHA-256 is
`c9ef547b9da38799203c4a245ad8707663d06e8bd90c132554c4f7338d353c7f`;
the manifest records the development patch against `97ba616c21`.
The serial log reports polling on both boots. This is emulated NVMe validation,
not Samsung SSD access or native RK3588 PCIe support.

`qemu_shell.py --nvme --power --normal` now provides this regression as a regular
gate with its own disposable namespace. Thirty-nine host checks pass, including
rejection of an unwritten or misplaced high region, corrupted data and a
truncated namespace. The clean source is
`2f2efd5c51d8ce40a514b4d7cf408605a6f16e50`; its private image SHA-256 is
`adae21d51f65d81a3d8edb15e3de88f2314c070367d5373dbc03c56eeaec1ddd`.

The first combined clean-image run,
`artifacts/qemu-shell/20260912T004022Z-8a2cc1`, failed before executing guest
tests: RNDIS initialization timed out waiting for its control notification.
That failed result remains preserved. A diagnostic repeat with QEMU xHCI
startup tracing passed in `artifacts/qemu-shell/20260912T004506Z-be4600`, including
memory and its negative control, platform, cache, service regression, 8 MiB USB
round trip and truncated-input rejection, all NVMe checks, normal reboot and
power-off. The trace is in
`artifacts/qemu-usb-notify/20260912T004506Z-9aafbc`. This successful repeat does
not explain or fix the intermittent RNDIS startup failure.

A separate run booted the same clean image directly from emulated NVMe, with
no USB boot disk. `artifacts/qemu-shell/20260912T004328Z-4c3846` records
`Mounted boot partition: /dev/disk/nvme/0/1` on both boots. Memory, cache, the
USB file round trip and negative checks, normal reboot and power-off passed.
The exploratory wrapper initially rejected the log because it counted the
SSD serial number in firmware messages as well as driver messages. The saved
`nvme-boot-review.json` checks the exact driver and mounted-partition markers;
the original wrapper error is retained. This qualifies a basic emulated BFS
boot through the NVMe driver, not installation or boot from the physical SSD.

## Read-only native UEFI PCI inventory

The extended EFI diagnostic reads UEFI PCI I/O and PCI Root Bridge I/O protocols.
It records device locations, the first 256 configuration bytes and the root
resource descriptors. QEMU validation in
`artifacts/efi-pci-qemu/20260912T005250Z-cde6bb` checked the emulated NVMe identity
and saved configuration bytes and verified that its separate virtual SSD was
unchanged. The source files and development patch against `0ca44d8dc3` are
preserved there. The tested diagnostic image SHA-256 is
`9c838892b82f8fac7fdd0a57832624190398a4c2455cf60cc3da76244852e019`;
the EFI application SHA-256 is
`532215cebc61b8fa4221a1b7a9510608299618f47a52ddda231b04362e16e76d`.

The same image passed a native deployment/inventory/recovery cycle in
`artifacts/hardware/20260912T005517Z-8be452`. Firmware exposes four PCI root
bridges and fourteen PCI protocol handles. Eight handles have valid vendor IDs:
four RK3588 bridges, Samsung NVMe, ASM1164 SATA and two RTL8125 controllers.
The other six report vendor ID `ffff` and are not counted as physical functions.
The Samsung is at segment 0, bus 1, device/function 0 with ID `144d:a802`,
class `010802`, revision 1 and PCIe link status reporting 8.0 GT/s, two lanes.
Its BAR0 is assigned `0xf0000000`; firmware reports a 2 MiB memory aperture
starting there for root segment 0. This differs from the saved Linux/mainline
device-tree resource arrangement. A Haiku handoff must account for the actual
firmware setup instead of treating the Linux device tree as live PCI mappings.

All device snapshots were read back from the copied USB image and matched the
logged identities. The copy's SHA-256 matched the detached NanoKVM file.
UART captured 186,546 bytes without errors; ROOBI returned with boot ID
`ed4cce1a-9c0f-4fe3-b64c-f532a8afcc51`. No PCI configuration, SoC register,
SSD, eMMC or SPI writes were requested by this inventory. The probe's files
were written only to its USB image. This establishes firmware discovery,
not native Haiku PCIe discovery or physical NVMe I/O under Haiku.

## Native PCI configuration after the firmware handoff

The installed EDK2 v1.1 source (commit
`6a682c0ef3ed74feb8b0d98f1c2aa771ddfbae18`) uses separate RK3588 root/endpoint
configuration mappings and filters invalid device slots. For segment zero,
the root is at `0xa40000000` and bus 1/device 0/function 0 is at `0x900100000`.
These are firmware mappings, not the Linux device tree's configuration window.
The new `rock5_pci_config_probe` reads only those two known functions through
read-only, uncached Haiku mappings. Its generated ARM64 loop uses individual
32-bit volatile loads. It does not write PCI configuration or program SoC
registers, clocks, address windows or DMA.

The first QEMU run, `artifacts/qemu-shell/20260912T011650Z-86bb19`, failed before
mapping any PCI page. The `poke` driver was packaged but could not load because
it required an ISA module absent from the ARM64 image. Source
`20a934e26caf2a1445af826382fc40dd88f5a5c0` makes its ISA/PCI module dependencies
optional, rejects operations requiring an unavailable bus and retains root-only
access. The initial failed evidence remains preserved.

The clean private image has SHA-256
`ec7d1ef97100b754112881f81b7a9b8f2d7fa04a751d6f13a8c2c50e0554b374`.
QEMU passed in `artifacts/qemu-shell/20260912T012011Z-d2825f`: host/NVMe
configuration reads before and after normal reboot, the separate NVMe fixture's
read/write/reboot/host-hash checks, 64 MiB memory with its negative control,
8,192 cache checks and normal power-off.

The same image passed the native probe before and after normal reboot in
`artifacts/interactive/20260912T012245Z-e698d4`. All 256 bytes of each root and
Samsung configuration snapshot matched the earlier UEFI inventory on both
boots. The Samsung retains ID `144d:a802`, BAR0 `0xf0000000` and PCIe 3.0 x2.
Both boots also passed a locked 64 MiB/eight-worker/two-pass memory check.
The inspected screenshot shows Tracker and Deskbar. Haiku's disk directory
still contains only USB and virtual devices: this proves configuration access
after ExitBootServices, not a PCI host driver or native SSD I/O.

ROOBI recovery passed with boot ID `71c3342d-95d3-421f-afb1-68d15e737dfb`.
UART saved 251,544 bytes without transport errors. The controller retained
boot ID `84b69ae8-4d9e-4c03-9f5c-6c9547084819` and its scoped watchdog disarmed
normally. `pci-config-review.json` preserves the parsed comparisons and raw
configuration files. Firmware handoff support, noncoherent DMA and PCIe
interrupt routing remain open before native disk qualification.

## ARM64 NVMe DMA buffer isolation

Source `6b8a1be864` adds a conservative ARM64 DMA path. Queue entries, PRP lists
and transfer buffers use private Normal Non-cacheable mappings. Allocation
cleans and invalidates the initial cached zeroing before changing the mapping,
as in the previously tested platform EHCI allocator. Device payloads use
preallocated 128 KiB buffers owned by command trackers, including admin
identification data and scattered block-I/O data. Copying follows the opcode's
transfer direction and preserves split-request offsets. Metadata payloads are
rejected rather than sent through an unimplemented metadata mapping.

The change adds full-system ordering before MMIO writes and after observing a
completion phase tag, reads that tag through a volatile access and bounds-checks
completion IDs. Coherent allocation alone would not provide ordering; see the
[Linux DMA guide](https://docs.kernel.org/core-api/dma-api-howto.html) and the
[Linux NVMe completion barrier](https://github.com/torvalds/linux/blob/v6.6/drivers/nvme/host/pci.c).
Eight data buffers are reserved per I/O queue and sixteen per admin queue;
the admin count leaves room for asynchronous event requests. This limits memory
use and avoids allocating new DMA areas while submitting I/O. Copying and the
smaller transfer/queue limits have a performance cost. This does not implement
an IOMMU, general PCI DMA translations, controller-stall recovery or per-device
coherence policy; other CPU architectures retain their existing payload path.

The development candidate passed the original NVMe checks in
`artifacts/qemu-shell/20260912T013856Z-ae3b4a`. Its recorded source patch and new
header are in `artifacts/nvme-dma-source/20260912T013855Z-ce6d9f`.
All forty host checks passed in `artifacts/control-checks-nvme-dma.log`.
The clean private image SHA-256 is
`6d1096c0fe01c5449e2a0df609ee63d263f6a2725e600b564db77b125d3a71af`.

The clean image passed in `artifacts/qemu-shell/20260912T014338Z-47339f`:
the original 8 MiB reads/write, five unaligned writes of 1, 513, 4,097,
131,073 and 1,048,579 bytes above 6 GiB, and complete 2 MiB surrounding-region
hashes after every write. All three regions matched again after normal reboot
and through independent host reads after normal power-off. The same run passed
the memory negative control, 8,192 cache checks and PCI configuration probes.
Serial records the noncached buffer pools on both boots.

`artifacts/qemu-shell/20260912T014540Z-839cbc` then booted the clean image directly
from emulated NVMe, with no USB boot disk. Both boots mounted
`/dev/disk/nvme/0/1`; memory/cache, an 8 MiB USB round trip and its truncated-input
check, normal reboot and power-off passed. These are functional emulation
checks. Physical Samsung DMA, storage performance and native SSD boot remain
untested until the firmware PCIe host handoff is implemented and qualified.

## First native Samsung NVMe I/O

Source `2d9159afce62b19faadb086e01a3da23ce85a7be` adds the explicit
[EDK2 v1.1 host profile](PCIE-FIRMWARE.md). It exposes only firmware segment 0,
retains the firmware's PCIe setup and checks the root/SSD configuration and
active link before the PCI core attaches. The existing Linux DT windows are
not used as live mappings. The opt-in setting is installed by the lab build.
Other PCIe ports, port I/O and MSI/INTx routing are not part of this profile.

All 41 host checks pass, including malformed profile/configuration cases under
sanitizers and the profile executable run against both captured firmware
configuration files. The clean private image SHA-256 is
`31be3e08dcb649c3ecc1d60b5afd1cbb89033c3d343157c1a9b041ed0989bee3`.
QEMU passed the NVMe sector guards, original I/O, normal reboot/readback,
independent host hashes, memory/cache and PCI config checks in
`artifacts/qemu-shell/20260912T021806Z-3cea25`.

On the ROCK, `artifacts/interactive/20260912T022104Z-706c6a` shows Haiku's
driver attaching to the Samsung 950 Pro on both boots, publishing
`/dev/disk/nvme/0/raw`, identifying the 512-byte namespace and using polling.
The 16 admin and 8-by-8 I/O tracker buffers use the ARM64 noncoherent path.
BAR0 remains CPU/PCI address `0xf0000000`, size 16 KiB. Tracker/Deskbar and the
USB shell remained usable; the eight-worker locked 64 MiB memory check passed.

The first native reads matched both independently generated Linux 8 MiB
fixtures at 2 GiB and 5 GiB. Haiku then wrote 8 MiB at 5 GiB using `dd conv=fsync`, seeded
a 2 MiB surrounding-byte test region at 7 GiB, and performed writes of 1, 513,
4,097, 131,073 and 1,048,579 bytes at unaligned offsets in that region. The full
2 MiB hash matched the host-generated expected bytes after each operation.
All three regions matched before and after normal Haiku reboot. After recovery,
Linux `O_RDONLY|O_DIRECT|O_EXCL` reads independently matched all three hashes.
The final surrounding-region hash is
`91d744cbe2803fbd788226912c73d56102634e1dbafe071cb70ecfa4da80f582`.
Expected bytes, scripts and Linux results are in
`artifacts/native-nvme-fixture/20260912T022046Z-338b9d`.

This is bounded physical read/write and warm-reboot evidence. Raw-device
`fsync()` did not flush the drive cache; see the correction below. It does not
qualify sustained performance, concurrent queues, controller-error recovery,
TRIM, high physical DMA addresses, native SSD boot or power-loss durability.
The logged DMA buffer addresses were below 4 GiB; large disk offsets do not
establish high-memory DMA. Both boots still used the NanoKVM USB boot volume.
The SSD's partition table is unchanged and its filesystem contents are disposable.

Recovery returned ROOBI boot ID `5a2d044f-2ea1-4d8b-9df5-ae5c6730bf40`.
UART captured 249,402 bytes without transport errors; the controller retained
boot ID `84b69ae8-4d9e-4c03-9f5c-6c9547084819` and its scoped watchdog disarmed.
`nvme-review.json` in the native artifact records the reviewed transcripts.

The same clean image subsequently booted directly from emulated NVMe on both
boots in `artifacts/qemu-shell/20260912T023150Z-6008aa`. Both kernel mounts were
`/dev/disk/nvme/0/1`; memory/cache, USB transfer and truncated-input checks,
normal reboot and power-off passed. This gates the first native SSD boot image.

## Native Samsung SSD development installation

The owner authorized erasing the entire Samsung drive. Installation evidence
in `artifacts/nvme-install/20260912T024056Z-90ec70` records the exact model,
serial and capacity checks, removal of its old GPT, and writing the tested
336 MiB private image above. A full Linux `O_DIRECT` readback matched the
source SHA-256. The resulting MBR contains a 32 MiB EFI partition and a
300 MiB BFS partition; most of the 256 GB drive remains unallocated. This is
a development installation, not a full-drive installation or release image.

Both physical trials used a one-time UEFI `BootNext` option with the full
Samsung namespace device path. The ordinary `BootOrder` stayed unchanged and
NanoKVM kept the ROOBI recovery image selected. Haiku's boot volume therefore
had to come from the SSD; UART and `df` both report `/dev/disk/nvme/0/1`.
The bootstrap image and firmware loader are unchanged from the QEMU gate.

- `artifacts/interactive/20260912T024625Z-d59135`: native SSD desktop and shell;
  eight-worker locked 64 MiB memory check; 16,384 instruction-cache checks;
  an 8 MiB file written and flushed to BFS. Normal Haiku reboot returned to
  ROOBI after firmware consumed the one-time boot option.
- `artifacts/interactive/20260912T025256Z-8dd22a`: a second one-time SSD boot
  read that existing file with matching SHA-256
  `e2641c6d7df3dc39fe21e02b32825f37ba0b6f76fa908b269882d4b917580abf`.
  The memory check passed again. Normal Haiku shutdown reached
  `PSCI: requesting system off`; a NanoKVM power-button command then started
  fresh DDR/SPL/EDK2 output and returned ROOBI.

The two UART captures contain 313,122 and 311,779 bytes with no transport
errors. HDMI capture timed out while powered off, and a short burst of
undecodable UART bytes followed the power-off marker. NanoKVM SSH remained
reachable with controller boot ID `84b69ae8-4d9e-4c03-9f5c-6c9547084819`.
Both scoped watchdogs disarmed. Final recovery boot ID is
`4b3a06db-b95e-4257-ad0e-ea291e12d62f`; review assertions and the per-trial
results are saved as `native-boot-review.json` in the installation evidence.

The persisted file was checked after reboot, before the subsequent power-off.
Concurrent/sustained I/O, controller-error recovery, TRIM, DMA above 4 GiB and
power-loss durability remain unqualified. The firmware-specific PCIe and
polling limitations still apply. BFS resizing is currently unimplemented, so
using the rest of the SSD requires a new larger filesystem and a proper copy
or installation, rather than growing this 300 MiB volume in place.

## Concurrent native NVMe I/O and USB control failure

The standalone `rock5_nvme_stress` probe at source
`726258c8d56e6744cb69a7529a0c308bc6e0905e` uses eight workers pinned across the
available Haiku CPUs. Each writes offset- and round-dependent patterns in
1 MiB requests, followed by raw-device `fsync()` and cross-worker reads in
reverse block order. That `fsync()` was a no-op, as diagnosed below. Host checks verify independent expected bytes, unchanged surrounding
data, read-only verification and corruption detection. The ARM64 binary SHA-256
is `85f353259e6d8eea216fed6da66d79ef1dc6a076827d0bb2e0b8ccae4d974f33`.
QEMU `artifacts/qemu-shell/20260912T031611Z-42d3cf` passed two rounds over
128 MiB, CPU placement, reboot readback and independent backing-file comparison.
The kernel/driver image remains the earlier SHA-256 `31be3e08...0989bee3`.

On the physical SSD, `artifacts/interactive/20260912T032043Z-2037fd` ran four
rounds over the unallocated 16–18 GiB range: 8 GiB written and 8 GiB verified,
with one worker on each of the eight CPUs. Every round and both 1 MiB guards
passed. The test took 35.409 seconds; each 2 GiB write phase took about
2.35 seconds and each verification phase about 6.50 seconds. These timings
include pattern generation/comparison and describe this bounded workload,
not a sustained performance qualification. The existing BFS file also matched
after the earlier normal power-off and power-button startup.

After the storage command completed, Haiku's EHCI/RNDIS/HID paths reported
USB transaction/checksum errors. A new shell connection closed before it
could request normal reboot. This trial remains **error**, despite its passing
storage transcript. It recorded 1,547 `Device check-sum error` messages.
NanoKVM SSH and watchdog heartbeats continued, with the same controller boot
ID. External recovery returned ROOBI `2ccc97af-b233-4b82-b251-e98e7049c1d6`;
the watchdog disarmed and UART transport itself stayed intact. NanoKVM's saved
kernel log includes DWC2 endpoint-stop timeouts during recovery. The cause of
the USB failure is unresolved; this does not establish that NVMe load caused it.

ROOBI then independently read all eight 256 MiB regions and both guards through
`O_RDONLY|O_DIRECT|O_EXCL`. Every hash matched the host-generated expected data.
The plan, source, binary, reference hashes, Linux scripts and results are in
`artifacts/nvme-stress/20260912T031610Z-16369c`. The obsolete 336 MiB installation
staging file was removed from ROOBI after its hash was verified; the immutable
workstation image remains available.

A subsequent SSD boot, `artifacts/interactive/20260912T032858Z-dc3cac`, read
the same 2 GiB from Haiku in 6.625 seconds, with all eight CPU placements,
patterns, guards, the probe executable and the BFS file checked. Normal Haiku
reboot returned ROOBI `973dfc55-a390-4c3f-b1bd-b7bce69e3392`. No USB checksum
errors appeared through that reboot. These are passing bounded concurrent
storage and reset-readback observations. Explicit flush qualification was
missing; the subsequent high-address DMA failure is recorded below. USB control
stability, longer
mixed workloads, high physical DMA addresses, error recovery, TRIM and
full-size SSD installation remain open.


## High-address NVMe trial and raw-device flush correction

Source `215a43a021913435a29f4b009fdff0af415d9fbb` adds an opt-in ARM64
allocation floor above 4 GiB, described in [PCIE-FIRMWARE.md](PCIE-FIRMWARE.md).
The private diagnostic image SHA-256 is
`a7fe3ac018dcffae29a9b7e7234e7cc7057dfb5dab394d5ba810c5d9fc202eb0`.
QEMU with 6 GiB RAM passed in `artifacts/qemu-shell/20260912T033950Z-a5ad84`,
with all ten logged buffer pools above 4 GiB across two boots. With only 2 GiB
RAM, `artifacts/qemu-shell/20260912T034228Z-1a1779` confirmed allocation failure,
failed namespace reads and unchanged backing data, while USB boot and reset
remained functional. A lazily published device entry can still exist after
attachment fails; successful I/O, rather than entry absence, is the criterion.

The native USB-boot trial `artifacts/interactive/20260912T034602Z-eda8a5`
recorded all eighteen buffer pools above 4 GiB across two boots. The same
8-worker probe `85f35325...d974f33` wrote and immediately verified four rounds
over 16–18 GiB, totaling 8 GiB each way, in 35.417 seconds. The surrounding
1 MiB guards matched. However, readback after normal Haiku reboot **failed**.
Independent Linux direct reads confirmed stale disk data. A complete read-only
2 GiB snapshot, its transfer/hash checks and sector analysis are retained in
`artifacts/native-nvme-high-dma/20260912T034405Z-f5f561`.
Exactly 192 KiB matches round three instead of round four: 16 KiB in each of
the first two worker regions and 160 KiB in the third. Every remaining sector
matches round four. The snapshot SHA-256 is
`dd783842e50b7647afef3d65251944f478c02f2e032e8be1a638ec65dbcad04c`.
The controller remained reachable and recovery returned ROOBI
`ec369cf5-3a67-4f59-a47e-22abf6c04926`. USB checksum errors appeared during
automatic recovery after the failed readback, not during the storage command.

Source inspection found that `devfs_fsync()` returns success without calling
the device driver. Therefore neither earlier raw `dd conv=fsync` commands nor
the original concurrent probe proved a drive-cache flush. Their recorded data
checks remain observations, but prior descriptions of raw-device flush
qualification were incorrect. BFS file syncing follows a different path and
can issue `B_FLUSH_DRIVE_CACHE`; the earlier SSD-boot trials may have benefited
from incidental filesystem flushes. That is an explanation to test, not an
established cause of the high-address failure.

The probe now calls `B_FLUSH_DRIVE_CACHE` explicitly for the raw namespace,
keeps `fsync()` for regular files, logs every flush result and fails on errors.
The QEMU `dd` test no longer reports a flushed-write result. Forty-six host
checks pass, including an injected flush failure that must stop before the
next write round. Emulator command-trace verification and a corrected native
write/reboot/Linux-readback trial are pending; high-address DMA persistence
is not yet qualified.


## Corrected native high-address DMA flush and persistence checks

The corrected probe at source `33d832c078a3236f1ab8907421dbb9e8179fffdd`
has ARM64 binary SHA-256
`8d826730f9490da896b1055df94f5a93213de87d60d02bf560a550eb6f6856e8`.
The kernel/private high-DMA image remains `a7fe3ac0...fc202eb0`, isolating the
probe's flush change. Forty-six host checks pass in
`artifacts/control-checks-nvme-flush.log`, including injected flush failure.

QEMU `artifacts/qemu-shell/20260912T042339Z-2affe3` passed two 128 MiB rounds,
reboot readback, independent backing-file hashes, normal shutdown, memory and
cache checks. The NVMe trace records exactly two namespace flushes and two
completion callbacks during the two rounds, with no flush from the preceding
raw `dd conv=fsync` checks. The selected events follow the actual
[QEMU 8.2.2 flush implementation](https://github.com/qemu/qemu/blob/v8.2.2/hw/nvme/ctrl.c).
Two preceding harness failures remain recorded: `20260912T041756Z-739a3f`
rejected a corrupted terminal/base64 upload before storage testing; binary
transfer replaced it. `20260912T042131Z-7d506b` completed the writes but rejected
an obsolete, unused callback trace event; the corrected check uses
`pci_nvme_misc_cb` alongside `pci_nvme_flush_ns`.

Native `artifacts/interactive/20260912T042647Z-9310c1` passed the corrected
four-round 16–18 GiB workload with one worker pinned to each CPU. All four
`B_FLUSH_DRIVE_CACHE` calls succeeded. It wrote and verified 8 GiB in 35.497
seconds, then verified the complete final 2 GiB after normal reboot in 6.644
seconds. Normal power-off, an 800 ms NanoKVM power-button pulse and a third
Haiku boot followed; all 2 GiB matched again in 6.845 seconds. The two 1 MiB
guards remained unchanged. All 27 logged buffer pools across these three USB
boots were above 4 GiB. This trial does not change the installed SSD image.

After recovery, Linux `O_RDONLY|O_DIRECT|O_EXCL` reads independently matched
all eight 256 MiB region hashes and both guards. ROOBI returned with boot ID
`7d6f1d18-478e-46b1-bbd5-5ee95dd0f28a`; the controller retained boot ID
`84b69ae8-4d9e-4c03-9f5c-6c9547084819` and its watchdog disarmed. UART captured
301,387 bytes without transport errors. All 77 USB checksum messages occurred
after the final passing readback during forced recovery. HDMI capture timed
out while the board was powered off. Neither observation invalidates the
completed storage checks or establishes a fix for the earlier USB outage.

The scripts, source, binary, QEMU references, Linux results and reviewed
checkpoint are in
`artifacts/native-nvme-high-dma-flush/20260912T041905Z-f547db` and indexed by
`state/native-nvme-high-dma-flush-checkpoint.json`. The earlier failure snapshot
is retained. Explicit flushing resolves this reproduction and qualifies the
bounded workload with high DMA addresses across normal reset and power-off.
These raw-device checks did not qualify sudden power loss, longer mixed
workloads, controller-stall recovery, TRIM, MSI or full-size SSD installation.


## Full-capacity SSD installation and large-file checks

The owner-authorized Samsung SSD was repartitioned as GPT with a 512 MiB EFI
partition at sector 2048 and a 255,522,242,560-byte BFS partition at sector
1,050,624. The BFS volume uses 4096-byte blocks and reports 238.0 GiB. Exact
model, serial, firmware, capacity, mount/swap state and partition geometry were
checked before formatting. The disk GUID is
`0eee8446-86b3-4e3a-8181-f49193b4968c`; EFI and BFS partition GUIDs are
`251a9ddb-8c8b-4935-876a-fe51d3e2f5f6` and
`f7e520a0-41fd-459a-9f92-e431d817faec` respectively.

The workflow was first rehearsed in QEMU
`artifacts/qemu-shell/20260912T043159Z-853d71`: Haiku formatted the partitions,
Installer copied the system, and the EFI loader and installed files were
verified. A sparse full-capacity image then passed two NVMe-only boots, a file
extending beyond 5 GiB, reboot readback, filesystem checking and normal shutdown
in `artifacts/qemu-shell/20260912T045749Z-a98c4a`. The original wrapper rejected
an overlong emulated serial label that the driver truncated. Its error remains
recorded; `fullsize-nvme-boot-review.json` checks the actual label, NVMe-only
configuration, two boot-volume markers and all passing core results. The
256 GB logical image is for QEMU only and was never uploaded to NanoKVM.

Native installation used the ordinary private USB image `6a2e3322...031bc7a7`,
built from kernel source `215a43a021913435a29f4b009fdff0af415d9fbb`.
`artifacts/interactive/20260912T050604Z-ee7d20` records Haiku's FAT32/BFS
formatting and Installer's successful copy. All eleven installed package hashes
and three required lab settings matched the source. The ARM64 EFI loader was
copied separately to `EFI/BOOT/BOOTAA64.EFI` and matched SHA-256
`ac0bc6ace649e94c3b67190c27e91cd733d8fda8349b31c3c6821f072f57f8db`.
`checkfs -c` checked 241 nodes without allocation errors. Both SSD volumes were
cleanly unmounted before recovery.

The full GPT device path is recorded in lab option `Boot0010`. Trials request
it through one-time `BootNext`, with the verified ROOBI USB selected and
`BootOrder` unchanged. This keeps the automated recovery route available.
The first two SSD sessions, `interactive/20260912T052301Z-b7462a` and
`interactive/20260912T053049Z-c5f388`, both reached the desktop and authenticated
USB shell with `/boot` mounted from `/dev/disk/nvme/0/1`. The first also passed
the locked 64 MiB, eight-worker memory probe and 16,384 cache checks.

A regular BFS file of 7 GiB + 8 MiB holds the storage test. Eight workers, one
per CPU, wrote and read four rounds over file offsets 5–7 GiB: 8 GiB written
and immediately verified in 36.524 seconds. Regular-file `fsync()` succeeded
after each round. The final 2 GiB independently matches the host-calculated
pattern SHA-256 `ae657b8cc8195eb7aacfe432772c9dcc8102f0e14ec5a4d010d263babcafc94b`.
Two surrounding 8 MiB guards also match. Normal reboot returned to recovery;
the next SSD boot verified the complete region in 6.733 seconds and repeated
the hash and guard checks. Filesystem checking found no allocation errors.
Normal Haiku shutdown and an 800 ms NanoKVM power-button startup subsequently
returned ROOBI, followed by another one-time SSD boot. This sequence checks
persistence across shutdown/startup with recovery intervening; it does not
establish direct SSD cold boot without that recovery step.

The third SSD session, `interactive/20260912T053644Z-187855`, verified the full
region, both guards and the host-calculated hash after shutdown/startup.
However, USB control failed during the subsequent filesystem check: UART
recorded 2,535 USB checksum messages, and the shell command timed out before
reporting completion. This remains an interrupted run, with its partial
transcript and successful automatic recovery retained. There was no observed
data mismatch, but the interruption does not qualify USB reliability.

The fourth SSD session, `interactive/20260912T054356Z-162715`, sent all final
check output to UART as well as returning a shell completion marker. Its full
2 GiB probe passed in 6.746 seconds, the independent hash and both guards
matched, and all eleven installed packages still matched their original
hashes. The filesystem check processed 254 nodes with no allocation errors;
the 47 indices and indirect block runs were checked. This session had no USB
checksum messages and completed a normal Haiku reboot. Each guarded session
returned to ROOBI with the NanoKVM controller still reachable.

The partition plan, scripts and receipts are in
`artifacts/nvme-full-install/20260912T044937Z-703399`; the reviewed checkpoint is
`state/nvme-full-install-checkpoint.json`. The earlier raw test ranges
at 2, 5, 7 and 16–18 GiB now lie inside the BFS partition and are retired in
`state/nvme-raw-fixtures-retired.json`. Their historical evidence is retained;
subsequent write tests must use identified regular files on the installed
filesystem. This is an experimental minimal installation with eleven packages,
the firmware-specific PCIe profile and polling. TRIM, controller-stall recovery,
MSI, sudden power loss, sustained mixed load and full-board parity remain open.


## NVMe TRIM range candidate

Source review found that subtracting the partial leading sector from a smaller
TRIM request could underflow. For example, offset 17 and length 1 with 512-byte
sectors should trim nothing; the old arithmetic produced a range subsequently
capped at `UINT32_MAX` sectors. The candidate normalizes only complete sectors
inside the requested and namespace intervals, compacts away empty ranges,
checks the 256-range command limit before narrowing the count, and checks
namespace DSM support before submission. Empty work succeeds without a command.

Forty-seven host tests pass, including more than two million interval cases
checked against a separate 128-bit arithmetic calculation. The ARM64 driver
and a QEMU-only ioctl probe compile. That probe requires an 8 GiB namespace
with a private test marker, so it rejects this physical Samsung drive.
The raw ioctl probe passes in ordinary and forced-high-DMA QEMU runs
`qemu-shell/20260912T060739Z-436e5d` and `qemu-shell/20260912T060739Z-d3c4c3`:
ten cases, exactly three nonempty DSM commands, exact surrounding bytes and
post-reboot readback. Earlier harness failures with QEMU's default discard
policy of `ignore` remain recorded; the corrected runs use `discard=unmap`.

The BFS test in `qemu-shell/20260912T061028Z-af5780` exposed a separate driver
issue. Although `fstrim` reported 8,025,243,648 bytes and allocated-file checks
passed, the controller trace skipped a 7,335,920-sector range exceeding its
advertised DMRSL of 4,194,303 sectors. This is a failed TRIM qualification;
`state/nvme-trim-dmrsl-failure.json` preserves the trace and review. GPT, EFI
and tail guards remained intact, and all ten DMA pools were above 4 GiB.

The follow-up candidate reads NVM command-set-specific controller Identify
limits and splits intervals to honor range count, per-range blocks and total
blocks per command. It retains intervals beyond `UINT32_MAX`, validates every
input before the first command, and waits for completion before reusing the
DMA descriptor buffer. Pre-1.2 controllers use legacy format limits without
sending an unencodable CNS selector. Optional Identify rejection on pre-2.0
controllers retains those defaults; other discovery failures disable TRIM
while retaining ordinary I/O. Nonzero limits also enable the mandatory DSM
support variant. Host interval and batch-limit checks and the ARM64 driver
build pass. Refreshed ordinary/high-DMA QEMU raw ioctl checks pass in
`qemu-shell/20260912T064028Z-63836b` and
`qemu-shell/20260912T064326Z-957939`. The BFS test passes in
`qemu-shell/20260912T064325Z-3230f4`, with CNS 06h / CSI 00h Identify traces,
five DSM ranges covering every requested block, and no skipped ranges. The
high-DMA BFS run `qemu-shell/20260912T064028Z-1aa8cf` also passes independent
review of those ranges, file checks, reboot, guards and ten DMA pools above
4 GiB. Its original wrapper error is retained: it expected a `nvme_notice`
message suppressed by the driver's library log level; the corrected wrapper
uses controller Identify traces. `state/nvme-trim-qemu-checkpoint.json` records
the combined gate. The image manifest records untracked host build libraries
created by checks; its tracked source patch is empty, and those libraries were
subsequently archived outside the source tree.

The field layout and processing semantics were checked against the
[NVM Command Set 1.0d specification](https://nvmexpress.org/wp-content/uploads/NVM-Express-NVM-Command-Set-Specification-1.0d-2023.12.28-Ratified.pdf),
with the skipped-range behavior checked in QEMU 8.2.2's `hw/nvme/ctrl.c`.
The previous physical raw fixture plan is retired; the next native test uses
BFS's free-space interface.


## Native BFS TRIM qualification

Driver source `5ac55e25f837c686b7489753312eacbdb59f733b` passed native
free-space TRIM from the diagnostic USB image SHA-256
`2d1b1f04c009f23607754ad70215bdef92376816243b26881d32230b050f15e4`.
In `interactive/20260912T064648Z-456e0c`, all nine NVMe DMA pools were above
4 GiB. The Samsung's full BFS volume was mounted at `/HaikuNVMe`;
`fstrim -f -v` reported 231,037,755,392 bytes, exactly its 56,405,702 free
4 KiB blocks. The existing 2 GiB pattern region, surrounding guards, eleven
installed package hashes, EFI loader and filesystem allocation checks passed
before and immediately after trimming. No USB check-sum errors appeared in
that initial TRIM/readback segment.

Normal reboot loaded the same diagnostic image and again allocated all nine
DMA pools above 4 GiB. The next command referenced a missing local script,
triggering automatic recovery before its readback could execute. That failed
session is preserved. Its serial capture also contains 59 USB transaction-error
messages after the second boot and before recovery; it does not establish USB
reliability. The preparation script now gives reboot requests and reboot
readback distinct filenames.

A subsequent one-time SSD boot in `interactive/20260912T065903Z-d24410`
reached the installed desktop with `/boot` on `/dev/disk/nvme/0/1`. Full
2 GiB verification passed in 6.744 seconds, the independent SHA-256 and both
guards matched, all eleven packages still matched, and `checkfs -c` reported
no allocation errors. That session captured no USB check-sum errors and
completed a normal reboot to ROOBI. Both controller guards disarmed.

Linux inspection before and after these trials used read-only direct I/O on
the identified, unmounted Samsung namespace. The primary GPT prefix, the entire
512 MiB EFI partition and the complete region after BFS containing the backup
GPT match exactly. The loader SHA-256 remains
`ac0bc6ace649e94c3b67190c27e91cd733d8fda8349b31c3c6821f072f57f8db`.
Scripts, raw results and the reviewed checkpoint are in
`artifacts/native-nvme-fstrim/20260912T064429Z-1c2c08` and
`state/native-nvme-fstrim-checkpoint.json`.

This qualifies successful filesystem free-space TRIM commands and preservation
across reboot/recovery; it does not measure physical NAND reclamation or sudden
power-loss durability. That SSD readback used the prior
`215a43a021913435a29f4b009fdff0af415d9fbb` build; the subsequent installed update
is recorded below. Controller-stall handling, sustained
mixed load, MSI, the other PCI roots and overall board parity remain open.


## Installed NVMe TRIM update

The physical Samsung installation now runs `hrev60097+57`, driver source
`5ac55e25f837c686b7489753312eacbdb59f733b`. Installer copied from the ordinary
private image SHA-256
`01c26d17692268e2f6dfe70792b217e2e0465f48e0e0b2c0022efe1282020191` onto
the existing full-capacity BFS volume. The GPT layout remains unchanged.
The installed image does not contain the high-DMA diagnostic setting.

The update rehearsal exposed a FAT overwrite problem: copying directly over
the read-only `BOOTAA64.EFI` returned `Operation not allowed` after truncating
the file to zero bytes. This occurred only in a disposable QEMU copy. The new
`install-efi-loader.sh` helper verifies the old loader, a separate backup and
a staged replacement before renaming the new file into place. QEMU checks
covered rejection of a wrong old hash, successful replacement and backup,
and an already-current retry. The new loader SHA-256 is
`31d8f11cef5a998ad2ef0a29608397dc1d1ce51cb8cfe4d6aea7eadfd1d9dc75`;
the retained previous loader is
`ac0bc6ace649e94c3b67190c27e91cd733d8fda8349b31c3c6821f072f57f8db`.

The complete QEMU installation repeat in
`qemu-shell/20260912T072836Z-41d159` passed the full 5 GiB + 8 MiB file hash,
eleven package hashes, filesystem checks, staged EFI replacement and explicit
sync/unmount. Independent cold readback of all packages and both EFI files
passed. The resulting full-capacity image then booted as NVMe-only storage in
`qemu-shell/20260912T075048Z-922b2c`; the full file and packages matched on
both sides of normal reboot, and normal shutdown completed. Memory, cache
and binary-transfer checks also passed. The transfer harness now resets its
two known fixture files in the disposable overlay, allowing reuse of an
installed image that retains those fixtures.

Failed rehearsals remain recorded. One whole-file hash exceeded the original
deadline before Installer started. A clone of the interrupted EFI-overwrite
run later had a mismatching `noto` package; that run had not reached its final
sync/unmount, and the exact cause is not established. Installation was repeated
from the original qualified disk and verified after clean shutdown. The first
updated-image boot test also stopped on a preexisting transfer-test file before
the corrected test setup was applied. These failures are not counted as passes.

On the board, `interactive/20260912T071040Z-13b11c` lost USB control after
the eight-worker precheck and before Installer launched; it recorded 13,118
USB check-sum errors and recovered to ROOBI. The fresh session
`interactive/20260912T074104Z-2c2277` completed Installer, a one-worker read of
the existing 2 GiB pattern region, its independent SHA-256, both 8 MiB guards,
all eleven package hashes, filesystem checks and the EFI update. Both volumes
were synced and cleanly unmounted. No USB check-sum errors were recorded in
that repeat. Using one reader here does not establish eight-core USB stability.

Two subsequent SSD boots in `interactive/20260912T075633Z-8ce009` and
`interactive/20260912T080235Z-7edb2d` mounted `/boot` from
`/dev/disk/nvme/0/1` with the updated kernel. On the first boot, installed-system
TRIM reported 231,038,304,256 bytes, matching 56,405,836 free 4 KiB blocks.
The full pattern region, guards, packages and filesystem checks passed before
TRIM and after normal reboot/recovery and the second SSD boot. Linux also
verified both EFI file hashes between those boots. Both native sessions
completed normal reboot to ROOBI, and their controller guards disarmed.
Neither recorded USB check-sum errors before its reboot.

Scripts and retained failure reviews are under
`artifacts/nvme-installed-update/20260912T070143Z-d7ec4d`; the final reviewed
checkpoint is `state/nvme-installed-update-checkpoint.json`. This qualifies the
installed update and the stated persistence checks. Sudden power-loss
durability, controller-stall recovery and overall hardware parity remain open.


## Explicit USB reset while booted from NVMe

The session tools now support a prepared, single-use NanoKVM USB reset for an
installed NVMe boot. Preparation verifies the boot receipt and UART mount,
uses an authenticated shell to sync and reject mounted USB filesystems, and
checks the selected and persistent recovery images. A changed target boot,
controller boot or image invalidates preparation. Failed reset operations
retain their error receipt and the session's normal recovery route. Nine new
host checks cover these interlocks; the complete 56-check suite passed.

The implementation uses NanoKVM's documented Reset HID operation. The
[2.4.3 implementation](https://github.com/sipeed/NanoKVM/blob/3b2ba7c0c1214f44da9d328f90bbdd025fac0413/server/service/hid/status.go)
invokes the installed USB script's `restart_phy` action. The local script was
read and archived before testing; it unbinds/rebinds the NanoKVM DWC2 device
and restores gadget mode. The configured recovery image remained selected,
and NanoKVM's boot ID did not change.

Native session `interactive/20260912T081718Z-9e2e7f` ran the qualified
`hrev60097+57` SSD installation, with the preceding ordinary-image QEMU gate
in `qemu-shell/20260912T081407Z-671b8c`. The reset removed and re-enumerated
the USB disk, HID and RNDIS device without rebooting Haiku. Keyboard and mouse
worked afterward. A Tracker prompt to mount the recovery FAT volume was
canceled. UART recorded 67 USB check-sum errors during disconnect and no more
after the new RNDIS device was added. A notification callback also recorded a
five-second control-request timeout during removal.

Networking did not return automatically. Through the KVM Terminal, `ifconfig`
showed only loopback, although `/dev/net/usb_rndis/0` existed. Explicitly running
`ifconfig /dev/net/usb_rndis/0 auto-config` restored `10.239.6.102`. Authenticated
commands and a filesystem check then passed. An 8 MiB upload took 17.81 seconds;
its staged return at 256 KiB/second took 53.68 seconds. Both copies matched
SHA-256 `7d212b9c884f5c77896de960ae17cc341cda43b14d6a971f34ca29ebd4badf7f`.
The target completed normal reboot to ROOBI boot ID
`f6dcbdce-97a0-435c-8066-3667219d1267`, and the controller guard disarmed.

This was a controlled reset of a working link. It establishes that USB reset,
KVM input and explicit network reconfiguration can preserve a running SSD
session; it does not qualify automatic reconnect or recovery from a spontaneous
controller outage. Evidence and source snapshots are in
`artifacts/native-usb-reset/20260912T081408Z-1af315` and
`state/native-usb-reset-checkpoint.json`. Automatic interface recreation is the
next investigation.
