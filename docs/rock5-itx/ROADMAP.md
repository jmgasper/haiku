# ROCK 5 ITX hardware roadmap

The goal is feature parity with a recorded Linux installation on this exact
board and its installed peripherals. ROOBI currently supplies inventory and
recovery; it is not yet a measured Ubuntu desktop baseline. Record kernel,
firmware, device-tree, peripheral models and test results before comparing
performance or declaring parity. Start with a stable headless system, then
desktop functionality, then multimedia and optional expansion.

Every row below is a separate actionable work item. A driver compiling, a PCI
ID appearing, or a firmware feature working is not acceptance. Record the
commit, image hash, firmware settings, raw logs and physical fixtures used.
Dependencies are phase numbers; work within a phase can be split further when
the underlying buses permit it. No calendar estimates imply guaranteed support.

Current owner priority: phase 7 GPU support, then remaining hardware. Further
Ethernet driver development is deferred. [GPU.md](GPU.md) records qualified
native power/reset/IRQ, firmware, client buffers, GPU VMs and persistent queues.
The +210 image runs Mesa/Panfrost on that native interface. Across two boots,
all 32,768 rendered pixels match Linux and all 32 Mesa submissions complete.
Allocation baselines, previous GPU regressions, both QEMU modes, recovery and
independent storage/image checks pass. [MESA.md](MESA.md) records the tested
scope, build recipe and retained first failure. The +215 image also qualifies
[EGL window rendering](MESA-WINDOW.md): all 84,480 bitmap pixels and 84,480
independently captured screen pixels pass across two native boots, including
resizing and retirement. All 36 window submissions complete; build/QEMU,
recovery and integrity checks pass. Presentation uses a CPU bitmap copy.
The +224 USB image qualifies [firmware tiler heap growth](MESA-HEAP-PRESSURE.md)
on two native boots. Four contexts each grow from one chunk to four, then
seven; all 24 firmware requests receive memory. All 51,992 pixels, 1,024 guards,
24 submissions and allocation baselines pass. Previous graphics regressions,
both QEMU modes, normal reboot/shutdown, recovery and independent integrity
checks pass. Refused allocations, incremental rendering, sustained workloads
and GPU fault/reset recovery remain open.

The +221 USB image qualifies [graphics-process cleanup](MESA-LIFETIME.md)
on two native boots. A process is terminated after completed rendering while
its resources remain open; the survivor renders correctly and a fresh context
works afterward. All 51,992 pixels, 1,024 guards and allocation baselines pass.
Earlier GPU/window fixtures, both QEMU modes, normal reboot/shutdown, recovery
and independent integrity checks pass. Termination during pending graphics
work, sustained rendering and GPU fault/reset recovery remain open.

The +219 USB image qualifies [six GLES pipeline operations](MESA-PIPELINE.md)
on two native boots: texture upload/sampling, depth, stencil, blending, scissor
and render-to-texture. All 155,976 pixels, 3,072 guards and sixty GPU submissions
pass, with allocations restored after each context. Both QEMU modes, earlier
GPU/window fixtures, normal reboot/shutdown, recovery and independent storage
integrity pass. Conformance, sustained work, graphics-process termination and
GPU fault recovery remain open.

The +217 image adds [normal OpenGL Kit rendering](MESA-GLVIEW.md): two live
BGLViews, desktop OpenGL 3.1, complete GL/screen pixels, resizing and retirement
pass across two native boots, with all 104 submissions completed. General
application compatibility, conformance, sustained rendering, actual firmware
OOM/growth, automatic GPU fault recovery and native display integration remain open.

## 0 — Build and lab foundation

Create the fork, pinned ARM64 toolchain, incremental image build, QEMU evidence,
checksum-verified NanoKVM deployment, recovery, local credentials and CI checks.
Accept only after a real image build, QEMU trial and hardware recovery trial.
Keep original firmware snapshots and all project storage on `/mnt/HaikuWork`.

## 1 — Firmware, serial and Linux baseline

Identify board revision from silkscreen; retain original SPI/eMMC snapshots and
make a complete restorable backup before firmware replacement. Verify the
ordered UART adapter's electrical levels and 1,500,000-baud operation, connect
RX/TX/GND to the documented UART2 header, and capture from reset through boot.
Test an ARM64 EFI route (candidate: board-specific EDK2) with reproducible
firmware settings and recover to ROOBI. Prefer a reversible external boot path
where the actual firmware permits it; do not assume a 2017 vendor U-Boot has
working `bootefi`, PXE or USB keyboard support merely from its environment.

Acceptance: logs from 20 reset/recovery trials, an EFI loader reaching its entry
point, and an independently usable restore procedure. Capture a Linux reference
inventory and per-device functional tests. Confirm SATA/M.2 lane sharing for
this revision before selecting storage fixtures. EDK2 ACPI is primarily tested
against Windows; evaluate actual ACPI and device-tree tables for Haiku.

## 2 — ARM64 kernel and RK3588 platform

| Work item | Starting point and dependencies | Acceptance |
| --- | --- | --- |
| CPU, MMU and memory | Existing `src/system/kernel/arch/arm64`; phase 1; firmware handoff, reserved RAM, cache attributes, exceptions, user/kernel address spaces | Single-core userspace boots; memory above 4 GiB verified; allocation, fork/exec and fault recovery stress without corruption |
| GIC, timers and SMP | Existing ARM64 interrupt code; GICv3/ITS, architected timer, PSCI; first establish one core | All four A76 and four A55 cores online; IRQ/IPI routing, monotonic clocks and 8-core load stable; reboot works |
| Device discovery and platform resources | FDT/ACPI bus managers; clocks, resets, power domains, pinctrl, regulators; keep firmware handoff documented | Correct resource graph and interrupt specifiers; known devices bind without hardcoded addresses scattered across drivers |
| DMA and IOMMU | ARM64 DMA/cache operations, bus address translations and per-engine IOMMUs | High-memory buffers work; sustained bidirectional transfers match hashes; invalid mappings fail predictably; no silent coherency failures |

## 3 — USB and interactive recovery

| Work item | Starting point and dependencies | Acceptance |
| --- | --- | --- |
| USB controllers and PHYs | Existing xHCI/EHCI/OHCI code; RK3588 DWC3/platform attachment and PHY/clock/reset glue; phase 2 | Enumerate each routed controller and hub; hotplug on four USB 3 Type-A, two USB 2 Type-A and both front USB 2 ports; test USB-C data separately |
| Keyboard, mouse and USB storage | Existing HID, SCSI and USB mass-storage drivers; USB controllers | NanoKVM HID works in Haiku; boot filesystem survives EFI exit; long hash-checked reads/writes on a disposable USB disk; recover after disconnect |

## 4 — PCIe, native networking and fast storage

| Work item | Starting point and dependencies | Acceptance |
| --- | --- | --- |
| PCIe host bridges | `src/add-ons/kernel/bus_managers/pci`; phase 2; RK3588 address windows, link training, INTx and MSI/MSI-X. The +156 firmware-profile driver handles an active-link training transition before SATA attachment on both USB and installed SSD boots; [scope](PCIE-TRAINING.md) | Correct config space/BARs and DMA for each root port; cold and warm boots; no dependence on firmware boot services |
| Both RTL8125 Ethernet ports | Existing `drivers/network/ether/rtl8125`; observed `10ec:8125` rev 05; PCIe and DMA | Each port obtains DHCP and works with static IPv4/IPv6; simultaneous sustained traffic, link changes and packet integrity; compare against a 2.5 GbE Linux peer; remote shell/file transfer reliable |
| ASM1164 and four SATA ports | ARM64 AHCI DMA and managed INTx support; `1b21:1164` initializes four direct ports across native reboot, and two-disk I/O passes QEMU; no physical SATA disk attached; [measured scope](SATA.md) | Test each port with identified scratch disks; filesystem/data hashes, flush durability, error recovery and simultaneous I/O; compare throughput to Linux |
| M.2 M-key NVMe | Existing NVMe driver; PCIe; Samsung 950 Pro 256GB identified in ROOBI with PCIe 3.0 x2 link; owner authorizes erasing this SSD for testing and eventual Haiku installation | Native namespace discovery; hash-checked I/O, flush/trim, error handling and repeated native boot; confirm lane/mux arrangement for this revision and qualify Haiku installation on this drive |

## 5 — Onboard and removable flash

| Work item | Starting point and dependencies | Acceptance |
| --- | --- | --- |
| Onboard eMMC | Verified eight-bit legacy SDR passes cached FAT file overwrites, explicit device-cache flushes, normal reboot and orderly shutdown/startup, including CPU buffers forced above 4 GiB. Linux independently verifies file/FS integrity and reference regions, and common SD/eMMC I/O passes QEMU. See [MMC.md](MMC.md) | Bounded four-writer cached I/O passes; qualify power-loss integrity, sustained I/O, error recovery, faster clock modes and Haiku boot; preserve the tested ROOBI recovery route |
| microSD | RK3588 SD/MMC host, card detect, regulator and pinctrl; phase 2 | Multiple known cards, insertion/removal, hash-checked scratch filesystem, recovery from I/O errors and boot where firmware allows |
| SPI NOR | RK3588 SPI/SFC attachment; observed 16 MiB loader device; phases 1 and 2 | Read and compare complete contents; expose geometry; separate read-only normal access from deliberate firmware updates; successful restore drill before writes |

## 6 — Board management and low-speed buses

| Work item | Starting point and dependencies | Acceptance |
| --- | --- | --- |
| GPIO, I2C, SPI, UART and PWM | Native platform bus drivers; phase 2; exact pinmux and exposed connectors | UART capture across reset plus native serial I/O; addressed I2C/SPI transfers with fixtures; GPIO edge interrupts and PWM measurements; no claims for unexposed SoC pins |
| Regulators, thermal, DVFS and fan | Observed RK806/RK8602/RK8603 devices and PWM fan; bus drivers | Valid voltage/clock transitions, temperature reporting, fan control/tach where wired, throttling under load and long-run stability; begin conservatively before performance tuning |
| HYM8563 RTC | Observed I2C `6-0051`; I2C and interrupt support | Read/set time, retain time with a fitted battery, alarm/wake where wired; verify drift against reference |
| Power, reset, watchdog and suspend | PSCI and board-specific power control; kernel and PMIC support | Software shutdown/reboot, front-panel events, watchdog recovery, suspend/resume and wake sources validated individually; repeated tests preserve storage |
| Hardware random and crypto engines | Observed Rockchip TRNG; discovery, clocks and DMA as needed | Entropy source health/error handling and integration; crypto known-answer tests before acceleration benchmarks; software fallback remains usable |

## 7 — Displays and GPU

| Work item | Starting point and dependencies | Acceptance |
| --- | --- | --- |
| Basic framebuffer and dual HDMI output | EFI GOP handoff first, then native VOP2/HDMI/PHY driver; phases 1–3 | Visible Haiku desktop and correct stride/pixel layout, then EDID, mode changes and hotplug on each HDMI output; two-display operation tested separately |
| USB-C data, Type-C control and DisplayPort | Observed FUSB302; I2C, USB, PHY and display work | Data and DP alternate mode in both plug orientations, connect/disconnect, negotiated power roles supported by board and monitor modes; verify actual connector routing |
| eDP, MIPI DSI and touch | Panel-specific timings, sequencing, backlight and I2C touch drivers; platform resources and display pipeline | Attach identified compatible panels; stable scanout, brightness, touch calibration/interrupts and suspend/resume on each tested path |
| Mali-G610 GPU acceleration | New Haiku kernel/userspace integration; evaluate compatible Mesa/Panthor/Panfrost pieces and licenses; MMU/DMA/power/display foundations | GPU submission and isolation, rendering conformance, accelerated desktop/applications and reset after GPU hang; software framebuffer is a separate earlier milestone |

## 8 — Audio

| Work item | Starting point and dependencies | Acceptance |
| --- | --- | --- |
| ES8316 analog and front audio | Observed codec; RK3588 I2S, DMA, clocks, I2C and Haiku multi_audio integration | Headphone and microphone paths plus front header playback/capture; levels, channels, sample rates, latency and sustained duplex operation measured |
| HDMI/DisplayPort audio | Display link and I2S/audio infrastructure | Enumerate sinks, stereo playback, hotplug/rate changes and A/V synchronization for both HDMI outputs and supported DP sink |
| S/PDIF output | RK3588 S/PDIF TX and clock/DMA driver | Valid output to an identified receiver at supported sample rates; channel correctness and underrun recovery |

## 9 — Capture and acceleration engines

| Work item | Starting point and dependencies | Acceptance |
| --- | --- | --- |
| HDMI input | Native receiver/PHY, DMA, audio capture and Haiku media integration | Capture unprotected test patterns and audio at negotiated modes; frame integrity, sync, source changes and long-run stability |
| Both CSI camera interfaces and ISP | Sensor-specific I2C/control, CSI/D-PHY, ISP and media integration | Install named camera fixtures; capture correct raw/processed frames from each interface, exposure controls, dropped-frame counts and timestamp accuracy |
| Video decode/encode and RGA | RK3588 codec/raster engines; DMA/IOMMU, firmware/userspace requirements and Haiku Media Kit adaptation | Known video corpus and image transforms match reference outputs; hardware-use evidence, throughput, resource cleanup and error recovery for each advertised mode tested |
| NPU | RKNN/runtime or alternative integration feasibility, firmware licensing and new kernel interface; platform DMA/power | Supported model outputs match a Linux reference within stated numerical tolerance; throughput, memory use and recovery tested; this is a substantial separate port |

## 10 — Optional expansion

| Work item | Starting point and dependencies | Acceptance |
| --- | --- | --- |
| M.2 E-key Wi-Fi | Discover actual module PCI/USB ID first; no module identified now; PCIe/USB and appropriate existing or new driver | Scan, associate, encryption, roaming, sustained traffic and suspend for the installed module; do not promise all M.2 cards |
| Bluetooth | Actual module's USB/UART transport and Haiku Bluetooth stack | Pair/reconnect, HID and selected audio/data profiles, coexistence with Wi-Fi and suspend; separate acceptance from Wi-Fi |
| Power configurations and accessories | ATX/DC and optional PoE module, front-panel wiring, expansion fixtures | Repeat system tests for each physically installed configuration; PoE power is not an OS networking feature; verify NanoKVM stays independently reachable |

## 11 — Integration and release qualification

The `+156` SSD update has passed package/configuration verification, two installed
boot/data/network/eMMC/recovery cycles and independent Linux eMMC readback.
Its first SSD boot handled the captured SATA training condition with a 1.6 ms
wait before AHCI attachment. [SSD-INTEGRATION.md](SSD-INTEGRATION.md) records
the current checkpoint and links to the earlier `+148` incomplete attempt;
[PCIE-TRAINING.md](PCIE-TRAINING.md) records the diagnosis and bounded fix.

Run cold/warm boots, 24-hour mixed CPU/storage/network/media load, memory checks,
power-management cycles, device hotplug and filesystem integrity checks. Compare
each hardware row with the recorded Linux baseline and document measured gaps.
Build a usable ARM64 package set: the current bootstrap-oriented repository
lacks many desktop/media packages, independently of driver completeness. Add
installation/update/rollback tests and a published known-good firmware/settings
matrix. A release claiming full support requires every installed/advertised
feature's acceptance evidence; uninstalled optional hardware remains untested.

## References and source starting points

- [Radxa board interfaces and revision-specific wiring](https://docs.radxa.com/en/rock5/rock5itx/hardware-design/hardware-interface)
- [ROCK 5 ITX product features](https://radxa.com/products/rock5/5itx/)
- [RK3588 EDK2 platform support and ACPI/device-tree caveats](https://github.com/edk2-porting/edk2-rk3588)
- This checkout's `src/system/kernel/arch/arm64`, `src/system/boot/platform/efi`,
  `src/add-ons/kernel/bus_managers`, `src/add-ons/kernel/busses`, and
  `src/add-ons/kernel/drivers` establish reusable Haiku code; they do not imply
  the corresponding hardware is already working on this board.
- The live Linux inventory and decompiled running device tree are preserved
  locally in `/mnt/HaikuWork/nanokvm/evidence`.
