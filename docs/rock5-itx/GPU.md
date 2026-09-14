# Mali-G610 development

GPU acceleration is the owner's current priority; further Ethernet driver work
is deferred. The working EFI framebuffer supplies a desktop, while native
display control and GPU rendering remain separate milestones. No accelerated
Haiku rendering or GPU firmware boot is claimed yet.

## Reference and integration route

ROOBI runs Linux `5.10.110-33-rockchip` and Mesa 20.3.5. Its vendor device tree
describes a 2 MiB GPU register window at `0xfb000000`, three interrupts,
SCMI/CRU clocks, a regulator and a power domain. The old Panfrost module fails
while parsing vendor operating points. Initial inventory finds no GPU driver,
no Mali device and an off power domain. The DRM nodes belong to the display
controller and NPU; they do not prove Mali rendering. The installed
`bifrost_kbase` CSF module panicked in the vendor power-domain driver while
enabling the GPU domain. Automatic recovery and independent storage checks
passed; that vendor trial remains failed.

A separate Linux 6.18.52 EFI image now boots from a built-in RAM filesystem on
the actual board. Mainline Panthor initializes the Mali-G610 and official
arch10.8 firmware, reports CSF interface 1.5.0, and passes GPU/interface queries
and empty GPU-VM creation/destruction. The GPU ID is `a8670005`, MMU features
`2830`, shader mask `50005`, with eight address spaces and eight CSF group slots.
The test then reboots normally to ROOBI. Its boot image, recovery image and eMMC
integrity checks pass, and the NanoKVM watchdog disarms. This proves a working
firmware baseline; no GPU job submission or rendering was tested. Linux used
temporary ignore-unused clock/regulator/power-domain settings for this reference.

EDK2's mainline tree uses `rockchip,rk3588-mali` / `arm,mali-valhall-csf`.
The actual Haiku device tree was captured on two +179 boots and is identical:
184,025 bytes, SHA-256
`d28f6e039a228ba655bc7e804c5fddaa843ec82d4cf965d79eec27972e74e574`.
Its GPU, CRU, PMU, GIC, clocks, IRQs and supply description match the new
resource admission profile. PCB revision is
v1.12; the public v1.11 schematic alone does not validate new regulator or
wiring assumptions. Do not read a powered-off GPU speculatively.

Mesa 25.3.6 has a Haiku EGL frontend and a Panfrost kernel-backend layer.
The current Haiku frontend selects software rendering. The Panthor path uses
DRM buffer objects, virtual mappings, synchronization objects, groups, queues
and tiler heaps; some CSF operations are outside the common backend abstraction.
The ABI assessment must include those direct calls. Enabling a Mesa build
option alone cannot provide Haiku GPU support.

## Firmware component

`CsfFirmware.cpp` validates the container and describes ordinary MCU memory
regions without allocating or accessing hardware. It checks table/entry bounds,
page-aligned ranges, overlap, backing-data sizes, supported section flags and
the writable shared interface. Table length differs from full file length;
distinct virtual regions may reference the same file data. Protected regions
are counted and excluded from normal mappings.

The parser holds caller-owned immutable bytes. Section copies initialize all
backing memory, including padding. The future DMA layer must perform cache
maintenance before device access. File, section-count and mapped-memory limits
are explicit loader policy rather than format limits.

`rock5_mali_firmware_probe` uses this implementation, reports the layout and
prepares CPU copies. Its marker records `gpu_started=0`. The host probe accepts
the official arch10.8 image: format 0.3, version hash `01050000`, 282,624 file
bytes, 792 table bytes, seven ordinary regions totaling 917,504 mapped bytes,
one protected region and 16 ignored metadata records. All 129 host tests pass.
The new production-code fixture uses ASan/UBSan and guarded memory to check
truncation, unsupported records, reversed/overlapping ranges, valid aliases,
limits, unaligned input, copy bounds, zeroed padding and failed reinitialization.
The ARM64 build and two-boot EL1/EL2 QEMU firmware checks pass. The first native
CPU preparation check also passes, but that two-boot controller failed to
schedule reboot because its log directory was absent. The board shut down
cleanly and recovered with unchanged recovery/eMMC hashes. Corrected scripts
open their `/tmp` log before launching the delayed command. Both CPU checks
pass in the corrected two-boot +179 repeat, together with all component/file
hashes, reviewed desktops, normal reboot/shutdown and independent recovery/eMMC
integrity. The failed first controller run remains recorded separately.

## Native resource interface

The +181 kernel component binds the RK3588 CSF device only after validating
the board, named interrupts, clock provider, power controller and supply
description. It resolves phandles instead of fixing their numerical values.
It exposes a pointer-free, read-only snapshot at `graphics/mali_csf/0`, checked
by `rock5_mali_resource_probe`. The +181 implementation performs no MMIO, clock, regulator, power,
interrupt-handler or GPU-memory operation. Its diagnostic marker records
`gpu_accessed=0`; the supply range is a firmware description, not a voltage
measurement. It supplies no display accelerant.

The production FDT traversal and ioctl run in a host fixture covering changed
phandles, missing/wrong providers, incompatible boards, malformed cell/string
arrays, incorrect IRQ routes, parent-reference cleanup and ioctl bounds. Guarded
pages check truncated and unaligned input. All 130 host tests, the ARM64 build
and both two-boot QEMU modes pass. Native attachment and the exact resource
snapshot pass on two boots, with invalid ioctl length and write-open rejection,
32 component and nine file hashes, reviewed desktops, normal reboot/shutdown,
and independent recovery/eMMC integrity. QEMU correctly exposes no GPU node.
An earlier +180 image used the legacy driver packaging directory; pre-native
review caught this and +181 uses the modern graphics module directory.

## Clock and power observation

The +182 opt-in diagnostic, `rock5_mali_resource_probe --platform`, reads CRU
clock selectors 158–160 and gates 66–67, plus PMU GPU idle request/ack/status,
software power-down request and repair status. Offsets come from RK3588 TRM
v1.0 Part 1, dated 2022-03-09. PMU offsets account for the actual FDT window
starting at `fd8d8000`. The GPU repair-complete indication is bit 1 in
`PMU_BISR_STS4`; idle and software power-down requests use bit 0.

It maps only the required CRU and PMU pages, with kernel read-only permissions,
and releases both on every path. It never maps the GPU, writes a register,
changes voltage, installs an interrupt handler or enables power. The fixed
64-byte result includes observation start/end times and raw register values;
the reads are sequential, not atomic, and do not measure voltage or frequency.
The host fixture tests actual production read offsets and cleanup, failed
first/second mappings and malformed requests using guarded read-only memory.
All 130 host tests, ARM64 build, both two-boot QEMU modes and the native trial
pass. Three samples on each of two native boots return identical register
values; each observation took 2–3 microseconds. Both desktop captures were
reviewed. Normal reboot, verified shutdown before media replacement, Linux
recovery, disarmed watchdog and independent recovery/eMMC integrity all pass.
The first launcher attempt failed its Python dependency preflight before
deployment; the corrected launcher explicitly uses the NanoKVM virtual environment.

| Register group | Native value on both boots | GPU interpretation |
| --- | --- | --- |
| CRU selectors 158/159/160 | `00001f80 / 00000000 / 00000000` | Source index 4 (SPLL), divide by 1; GPU PVTPLL selection is off |
| CRU gates 66/67 | `00000004 / 00000000` | GPU clocks are enabled at the CRU; only the test-output gate is disabled |
| PMU idle request/ack/status | `00000000 / 00000fff / 00000fff` | GPU software idle request is clear; idle and acknowledge are set |
| PMU software power-down request | `0000fff9` | GPU power-down request is set |
| PMU repair status 4 | `ffff8001` | GPU power-up/repair-complete indication is clear |

The matching device tree describes SPLL as 702 MHz. Its requested 200 MHz GPU
assignment has not been applied by a native Haiku SCMI/clock driver. These
values establish a powered-down starting state and the selected clock setting,
not a measured GPU frequency or voltage. The next power trial must choose a
conservative GPU divider, verify power/idle handshakes before a GPU read and
restore its changes after the diagnostic.

## Next milestones

The new `rock5_mali_resource_probe --identity` diagnostic implements the next
power cycle. It is disabled by default; the private image must explicitly set
`firmware_profile rock5-itx-edk2-v1.1-gpu-identity` in the `mali_csf` driver
settings. Admission additionally checks the fixed SPLL description and GPU
power-domain child, its clocks and matching boot-on supply. Node names are
checked as direct children and phandles remain dynamic. The existing EDK2
regulator initialization requests 750,000 microvolts for the GPU supply;
this first diagnostic inherits that policy and does not measure or change it.

The sequence changes only the GPU divider to four (described 175.5 MHz),
requests idle, powers the domain, waits for repair completion and de-idles it.
Only after checking the resulting state does it map the GPU page read-only
and compare static identity/features with the Linux reference. It unmaps the
GPU before idling/powering down, then restores the initial idle request and
divider and compares all ten platform registers. Shared PLLs, clock gates,
regulators, GPU commands, firmware and DMA are untouched by this diagnostic.
Each poll has a 10 ms deadline and an iteration limit. Failed handshakes and
restoration are retained independently; uncertain restoration blocks further
identity cycles until recovery. Platform observations share the same lock.

The production sequence passes host models with delayed handshakes, failed
power-up/de-idle/power-down, failed clock restoration, GPU mapping failures,
wrong identity, unexpected initial state, and a stopped/backwards timer. The
production driver/FDT fixture also checks additional profile rejection,
default-disabled and recovery-required ioctls, mapping permissions and cleanup.
ARM64 build, QEMU and native identity access are pending.

1. Establish a conservative GPU clock and bounded power/identity cycle using
   the recorded native starting state. Then implement reset and interrupt
   delivery, with regulator ownership and restoration explicitly accounted for.
2. Implement GPU page tables, backing-memory lifetime and cache operations.
   Load the validated regions and verify the MCU boot handshake, interface
   version, timeout cleanup and normal reboot.
3. Implement the selected Mesa CSF kernel operations. Run a checked GPU memory
   operation, then an offscreen rendered image.
4. Integrate Panfrost with Haiku EGL/OpenGL and window output; qualify pixels,
   multiple contexts, process exit, reset, sustained work and conformance.
5. Implement native VOP2/HDMI modes/hotplug and additional display routes,
   followed by other board hardware.

## Sources and evidence

- [Panthor v6.12](https://github.com/torvalds/linux/tree/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/panthor),
  revision `adc218676eef25575469234709c2d87185ca223a`. Relevant driver files
  offer GPL-2.0 or MIT licensing; the container layout follows `panthor_fw.c`
  under its MIT option.
- [Mali CSF binding](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/Documentation/devicetree/bindings/gpu/arm%2Cmali-valhall-csf.yaml).
- RK3588 TRM v1.0 Part 1 (2022-03-09), local
  `artifacts/reference/rk3588-trm-v1.0-part1-20220309.pdf`, SHA-256
  `52fd969a33c0cfc4b14e90fb395c7bc71ac0132a1f65ab0e827e9b63c3404bc3`.
- [Mesa Panfrost](https://docs.mesa3d.org/drivers/panfrost.html), Mesa 25.3.6
  archive SHA-256 `59217efeac3b64e7ced958324b9db7494f1e0741aeb22d780276514cc1b8f206`.
- [HaikuPorts recipe and patches](https://github.com/haikuports/haikuports/tree/50ed75230b0be025c78f99a47508d1825799677d/sys-libs/mesa),
  revision `50ed75230b0be025c78f99a47508d1825799677d`.
- [Installed EDK2 v1.1 source](https://github.com/edk2-porting/edk2-rk3588/tree/6a682c0ef3ed74feb8b0d98f1c2aa771ddfbae18).
- [Linux 6.18.52 source](https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.52.tar.xz),
  archive SHA-256 `2b69564f7d4fea0c859b1959ba33709ee6e9139bd100e30a853b57159a8221b8`.
  Native firmware/VM/reference qualification:
  `artifacts/native-linux-gpu-reference/20260914T210244Z-af8ef5/qualification.json`.
  The 96 MiB EFI/RAM image SHA-256 is
  `2f1b2e90cb5706b0238fd40e523c2c38e3ea8876b4da24058fb99641423b1b69`.
- [Official arch10.8 firmware](https://gitlab.com/kernel-firmware/linux-firmware/-/blob/eeccccbe83daf22e1931e3557ba05b2c02427e4e/arm/mali/arch10.8/mali_csffw.bin),
  revision `eeccccbe83daf22e1931e3557ba05b2c02427e4e`, SHA-256
  `a27847ea11f8efb3136340c3ba8aab413ae25145eeb4a7f64ff5edd829a2405b`.
  Its separate `LICENCE.mali_csffw` is retained with the local binary.

The local index is `/mnt/HaikuWork/state/gpu-bringup-plan.json`. Initial
privileged inventory, source hashes and host evidence are under
`artifacts/gpu-bringup/20260914T201454Z-f2ee41`. Firmware, large sources and
images stay beneath `/mnt/HaikuWork`; firmware binaries are not committed.

Qualified native component evidence:

- +179 firmware preparation:
  `artifacts/automated-mali-firmware/20260914T211726Z-fe9d49/qualification.json`.
  Image SHA-256 `170c83873c0a3fa3509e9728833451d80976979df972c66369a47d101264d478`.
- +181 resources and firmware preparation:
  `artifacts/automated-mali-resources/20260914T213557Z-abfb48/qualification.json`.
  Source `ef7d4b9f3553059ab1debeb6288a106626c82277`, image SHA-256
  `4065a65e41b23b1f8828d3a466041af05096c60b1475dcbb6bf00029b01346fc`.
  Both native trees retain the previously recorded FDT hash. The recovery
  controller stayed up and its watchdog disarmed; no panic or USB filesystem
  checksum error occurred. This component qualification does not resolve the
  separate EL1 profiler/fault or installed SSD page-aging qualification.
- +182 read-only clock/power observation:
  `artifacts/automated-mali-platform/20260914T220117Z-9df5b3/qualification.json`.
  Source `8b2cd1e11c804a878b01b47f65ed86228ffd9262`, image SHA-256
  `9ebd436f31ad82d7c9a3bd8006699fb83a561f44e82bf5c78d888127af1330b2`.
  EL2 and EL1 QEMU evidence: `artifacts/qemu-shell/20260914T215553Z-42d745`
  and `artifacts/qemu-shell/20260914T215553Z-a01013` respectively.
