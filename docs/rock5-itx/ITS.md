# Initial RK3588 ITS/LPI profile

The first ITS implementation is confined to ITS1, the Samsung 950 Pro at
firmware segment-zero BDF `01:00.0`, and CPU 0. It is enabled only by
`home/config/settings/kernel/drivers/gicv3_its` containing:

```
firmware_profile rock5-itx-edk2-v1.1-dt-its-nvme
trace true
```

The setting is absent from ordinary images and the installed SSD. Do not combine
it with the experimental MBI provider. It requires the captured EDK2 v1.1 DT
profile, ITS1 resources, segment-zero identity `msi-map`, eight CPUs and the
measured GIC-600 capabilities. An unrelated QEMU machine must reject it before
ITS MMIO. AHCI remains blocked and RTL8125 absent in initial native trials.

The [earlier MBI experiment](MBI-PROBE.md) delivered CPU-generated messages but
failed real SSD MSI-X delivery. Linux instead programs `0xfe670040`, with event
IDs 0 through 8. A 64 MiB EFI read generated 513 NVMe interrupts and matched
the retained hash. Read-only Linux capability capture
`linux-its-reference/20260912T221705Z-a8f64c` then recorded both ITS instances
with IIDR `0x0201743b`, TYPER `0x130001ef31`, device-table entry size 8 and
collection-table entry size 2. ITS1's command queue drained normally. CPU 0's
Redistributor TYPER was `0x21`. This is a Linux baseline, not native Haiku proof.

Implementation references are Arm IHI 0069G
[chapters 5 and 8, and register descriptions in chapter 12](https://www.scs.stanford.edu/~zyedidia/docs/arm/gic_v3.pdf),
Rockchip's [RK3588 TRM Part 1, chapter 11](https://www.scs.stanford.edu/~zyedidia/docs/rockchip/rk3588_part1.pdf),
and the pinned Linux v6.12
[RK3588001 handling](https://github.com/torvalds/linux/blob/v6.12/drivers/irqchip/irq-gic-v3-its.c).
The SoC's table masters address 35 bits and require non-shareable LPI-table
accesses. Haiku allocates private contiguous pages below 32 GiB, removes the
allocator's cached lines and retypes the mappings to Normal Non-cacheable,
following the qualified NVMe DMA approach. Command, device, collection,
property, pending and interrupt-translation tables remain kernel-owned.

Initialization refuses active LPIs on any Redistributor and requires ITS1 to
be disabled and quiescent before writing table descriptors. Device and
collection tables are flat, with 64 KiB pages; the command queue is 64 KiB.
Register readbacks must match. CPU 0 receives its own pending table; property
IDbits is 13, covering hardware IDs through 16383. Kernel vector storage now
covers that range and dispatch decodes the complete 24-bit IAR. IDs 8192 through
8223 are reserved for this initial profile; LPIs use edge-triggered dispatch.
Other CPU Redistributors remain without LPI tables.

The PCI core now accepts optional host attributes for the MSI controller and
a contiguous requester-ID translation. The existing allocation API remains
available, and existing MSI providers default to their old allocation method.
Only the Samsung host exports this description initially. Haiku's domain
numbers reflect enumeration order. Moreover, EDK2 assigns secondary bus 1 to
multiple roots, while Linux assigns separate bus ranges: blindly adding the
DT's other segment bases is not evidence of the physical ITS DeviceID.
SATA/Ethernet routes need their own verified requester-ID contract.

The provider accepts only ITS1 DeviceID `0x100`, leases 1 through 32 vectors to
that device and maps event IDs starting at zero. MAPD/MAPTI establish the
translations; property changes use INV followed by SYNC. Submissions are
serialized with bounded polling and command-reader checks. Invalid/live frees
are retained; a normal free requires the PCI source disabled and all handlers
synchronously removed, then CLEAR/DISCARD/SYNC and invalid MAPD. A command or
descriptor failure quarantines the resources until reboot so hardware cannot
access freed/reused tables. No allocation occurs under the interrupt spinlock.

Host tests check known command packets, queue wraparound, whole-buffer DMA
limits and firmware/requester rejection, including the actual captured DTB.
The first full ARM64 build passed, along with 73 host checks. QEMU and native
validation remain pending. Native acceptance requires real SSD interrupt
counts during hash-checked I/O, no polling fallback, normal reboot, component
hashes, filesystem checks and verified recovery. Allocation/free/reuse,
multi-vector devices, other ITS instances and CPU affinity remain separate
qualification work.
