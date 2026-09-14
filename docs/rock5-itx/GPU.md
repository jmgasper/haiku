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
pass in the repeat; final recovery and integrity qualification are pending.

## Native resource interface

The next kernel component binds the RK3588 CSF device only after validating
the board, named interrupts, clock provider, power controller and supply
description. It resolves phandles instead of fixing their numerical values.
It exposes a pointer-free, read-only snapshot at `graphics/mali_csf/0`, checked
by `rock5_mali_resource_probe`. It performs no MMIO, clock, regulator, power,
interrupt-handler or GPU-memory operation. Its diagnostic marker records
`gpu_accessed=0`; the supply range is a firmware description, not a voltage
measurement. It supplies no display accelerant.

The production FDT traversal and ioctl run in a host fixture covering changed
phandles, missing/wrong providers, incompatible boards, malformed cell/string
arrays, incorrect IRQ routes, parent-reference cleanup and ioctl bounds. Guarded
pages check truncated and unaligned input. ARM64 build and native attachment
are pending.

## Next milestones

1. Capture Linux GPU identity and actual Haiku resources; establish clock,
   regulator and power-domain ownership, bounded reset and interrupt delivery.
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
