# EDK2 v1.1 PCIe development profile

The initial host driver supports only the Samsung 950 Pro on firmware segment
0 of this ROCK 5 ITX. It retains EDK2's link, clocks, PHY and address translation
setup after ExitBootServices. It does not initialize those resources itself.
This is a firmware-specific bring-up path, not generic RK3588 PCIe support.

The opt-in setting in `home/config/settings/kernel/drivers/rk3588_pcie` is:

```
firmware_profile rock5-itx-edk2-v1.1-dt-samsung950
```

Only the lab UserBuildConfig installs that setting. It asserts that the board
is running the installed EDK2 v1.1 release in mainline DT-only mode. The driver
does not infer a firmware version from a PCI ID. Changing firmware or table
mode requires rechecking this profile before enabling it. The FDT board ID,
root register range, root/endpoint IDs, classes, single-function headers,
firmware bus numbering, memory decode and BAR containment are checked before
the PCI core attaches. Unsupported slots/functions are rejected without MMIO.

Source contract: edk2-rk3588 commit
`6a682c0ef3ed74feb8b0d98f1c2aa771ddfbae18` (v1.1), and its edk2 submodule
`fbe0805b2091393406952e84724188f8c1941837`:

- `Rk3588Pcie.h` / `PciSegmentLib.c`: root DBI at `0xa40000000`, bus 1 config
  at `0x900100000`; reject device numbers above zero on buses 0 and 1.
- `Rk3588PciHostBridgeLib`: firmware config/iATU setup and MEM32 aperture at
  `0xf0000000`. The driver exports only the subset currently forwarded by the
  root's standard PCI memory base/limit registers (1 MiB in the captured setup).
- `RK3588Base.dsc.inc`: PCI uses `NonCoherentIoMmuDxe`.
  `Rockchip.dsc.inc`: `DmaLib` is `NonCoherentDmaLib`.
  Its `HostToDeviceAddress()` adds `PcdDmaDeviceOffset`, whose default is zero
  in `EmbeddedPkg.dec`; this platform has no PCI override. This is a software
  DMA/cache implementation, not a hardware SMMU mapping service.

The mainline DT describes the resources Linux would program. Its config and
memory windows differ from EDK2's live setup. Its `iommu-map` points to the
SMMUv3 at `0xfc900000`; the explicit profile checks that description and keeps
the firmware's identity DMA handoff. Haiku currently has no SMMUv3 driver that
changes it. Introducing such a driver requires revisiting this contract.
No SMMU, PHY, pin, clock or iATU registers are written by this host driver.

Configuration uses Device-nGnRnE mappings, sized volatile accesses and full
system barriers. The ordinary PCI core still performs standard configuration
writes, including command bits and BAR sizing. Only the root and the Samsung
endpoint are exposed. Port I/O, prefetchable windows, other root ports, hotplug,
INTx routing, MSI/ITS and controller reinitialization remain unsupported. The
NVMe driver uses its ARM64 polling path and noncoherent DMA buffers.

The profile test accepts the actual captured 256-byte root and endpoint files,
checks the expected window, and exercises invalid bus/device/function/offset,
identity, class and BAR cases with sanitizers. Ordinary QEMU tests cover the
shared PCI/NVMe stack and the new driver's refusal to bind to unrelated hardware;
they cannot emulate the RK3588 host. Native hash checks and reboot tests are
required before calling physical storage functional.
