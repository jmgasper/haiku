# Bounded GIC message-interrupt diagnostic

`rock5_mbi_probe` is a separate ARM64 lab driver. It is built explicitly with
`jam '<driver>rock5_mbi_probe'` and is absent from ordinary images. Opening
`/dev/misc/rock5_mbi_probe` runs one short diagnostic; reads return its result.
It does not publish an MSI interface or change PCI endpoint configuration.

The private test image must install the driver and its device symlink, and set
`home/config/settings/kernel/drivers/rock5_mbi_probe` to:

```
firmware_profile rock5-itx-edk2-v1.1-dt-mbi-test
```

The driver requires root, this exact setting, the ROCK 5 ITX/RK3588 compatible
strings, the captured GIC register ranges, four interrupt-description cells,
the firmware MBI range and alias, and no active global MSI provider. Mismatches
fail before MMIO. Controller reads then require the measured GICv3 TYPER with
MBIS support, enabled Group 1 forwarding and affinity routing, the expected
non-secure priority, and an inactive, disabled test interrupt. The group bit
is checked only when the single-security-state view exposes it. The selected
trigger field and interrupt enable must read back before any message is sent.

The only test vector is absolute interrupt ID 464. The captured DT advertises
IDs 424 through 479 for MBI, but many earlier IDs overlap wired devices.
[Rockchip's RK3588 TRM v1.0, Part 1, Table 1-3](https://www.scs.stanford.edu/~zyedidia/docs/rockchip/rk3588_part1.pdf)
reserves IDs 454 through 511. This probe does not enable the whole DT pool.
It retains the kernel's existing CPU 0 route and priority for ID 464.

The probe temporarily selects edge triggering, installs one synchronized
handler and verifies a quiet interval. It sends four writes to GICD_SETSPI_NSR
at distributor offset `0x40`, then four through the DT alias at `0xfe610000`.
Each write must produce exactly one interrupt on CPU 0 within 500 ms. A final
quiet interval rejects extra interrupts. Handler removal disables delivery and
synchronizes with any callback. Cleanup waits for the active state to clear,
clears only this vector's pending bit, restores its trigger field and verifies
the result before releasing the semaphore and mappings.

Register semantics follow Arm's
[GICv3/v4 software overview](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/GICv3_v4_overview.pdf?revision=65f91645-cd52-4795-952b-f01095ff5ef8)
and the pinned Linux v6.12
[MBI implementation](https://github.com/torvalds/linux/blob/v6.12/drivers/irqchip/irq-gic-v3-mbi.c).
The latter uses absolute interrupt IDs as message data and defaults to edge
triggering. These references do not establish working PCIe message delivery on
the board. A CPU-generated test is an earlier, separate milestone.

Host sanitizer tests accept a synthetic table and optionally the captured DTB
through `ROCK5_MBI_CAPTURED_DTB`. They reject changed board/resource descriptions,
absent MBI properties, missing MBIS support and enabled/pending/active vectors.
QEMU must demonstrate rejection before MMIO, along with the existing boot,
NVMe, USB and reboot gates. The board and register predicates are shared with
the opt-in kernel MBI provider; the diagnostic still rejects an active provider.

The first native attempt, `interactive/20260912T144340Z-fd001c`, rejected the
controller before writes because the original predicate expected a visible
Group 1 bit. TYPER and PIDR2 matched, and the vector was inactive/disabled, but
IGROUPR read zero. Arm IHI 0069G,
[section 12.9.13, GICD_IGROUPR](https://www.scs.stanford.edu/~zyedidia/docs/arm/gic_v3.pdf),
specifies zero reads for non-secure access when DS is zero. The corrected
predicate distinguishes the two register views and also checks the priority
and enable readback. It does not change security grouping or GICD_CTLR.
The original QEMU rejection gate passed in `qemu-shell/20260912T144020Z-e72eeb`.
Native recovery returned ROOBI `5cdbe255-0734-44b6-98bc-bc884e1047ab`, with
the guard disarmed. This attempt did not test message delivery.

The corrected diagnostic from `5e961c4459`, binary SHA-256
`6ae2526a7afe2f375a28d2f6e1eab525238dd3401645433a9109b4de4cc4435b`,
passed QEMU in `qemu-shell/20260912T145447Z-7b7d61`. Its private USB image,
SHA-256 `fef7982f326c345cbc44f082af190f7c6efbd813876e36d99309ed57289a124a`,
then passed native session `interactive/20260912T211720Z-6d22f2`.
Two runs on the first boot and one after a normal Haiku reboot delivered all
24 expected messages on CPU 0: 12 through each address. Every run passed the
quiet intervals and disabled/inactive/restored cleanup checks. The measured
control register was `0x12`, group register zero and priority `0x80`.

Both boots passed all five component hashes, eight PCI function identities,
a 64 MiB read of the NVMe EFI partition matching the Linux reference, strict
zero BFS allocation counters and one RNDIS notification worker. No USB checksum
or control timeout errors occurred in either workload window. Authenticated
access returned 92.244 seconds after the normal reboot test started. Recovery
returned ROOBI `adf214ad-c5dc-4348-85b0-efe008f0aade`; the controller guard
disarmed and NanoKVM's boot ID was unchanged. `state/native-mbi-probe.json`
indexes the evidence. The installed SSD components were unchanged.

## Kernel MSI provider trial

The ARM64 GICv3 controller can now provide the existing MSI allocation API for
the same exact firmware description, with a separate explicit setting in
`home/config/settings/kernel/drivers/gicv3_mbi`:

```
firmware_profile rock5-itx-edk2-v1.1-dt-mbi
trace true
```

The provider leases IDs 464 through 479, chooses an aligned base for MSI,
also supports non-power-of-two MSI-X counts, and returns `0xfe610040` with the
absolute interrupt ID as message data. All 16 IDs must pass the controller
checks before their trigger fields are changed. They keep CPU 0 routing and
use edge semantics in the kernel handler. Allocation is serialized; incorrect
frees and still-enabled/active vectors cannot be reused. Trace mode reports
the first eight interrupts per vector and later power-of-two counts.

This setting is absent from ordinary images and the installed SSD. Enabling
it also lets the existing NVMe driver select MSI-X automatically. The first
USB trial kept AHCI blocked and RTL8125 absent. CPU-generated message delivery
does not accept PCIe MSI, ITS, Ethernet operation, or the complete firmware
MBI range.

The provider's first native image from `6a613b204d` passed QEMU in
`qemu-shell/20260912T213553Z-9f5c4b`, but failed real message delivery in
`interactive/20260912T213902Z-a71c6d`. NVMe enabled MSI-X and allocated ID 464
with address `0xfe610040`; no GIC message interrupt arrived. One interrupt
timeout caused the existing NVMe fallback to select polling. The 64 MiB EFI
read then matched the Linux reference, all five component hashes matched,
the eight PCI functions remained visible, and filesystem checks were clean.
The native MSI-X/reboot milestone has not passed.

`rock5_msi_inspect`, built as a separate lab driver, reads that state through
read-only Device mappings. Its settings file must specify
`firmware_profile rock5-itx-edk2-v1.1-dt-msi-inspect`. It requires the exact
board/GIC firmware description, an active MSI provider, a live Samsung root
link and the captured SSD BAR/MSI-X layout. It reads the first table entry,
pending bitmap, controller status, ID 464's GIC state and the PHP_GRF ITS
address-match/TBU registers documented in the TRM. It has no MMIO write path.
Host tests reject changed SSD identities, BARs and MSI-X layouts. QEMU rejected
opening `/dev/misc/rock5_msi_inspect` before MMIO and passed the existing
storage/USB/reboot gates in `qemu-shell/20260912T215411Z-b066fc`. The inspector
from `29f14a00b0`, SHA-256
`cc6e84bf71c6dd2bf15814cd10642fad870823d2f688bb31b1fd4bae5bc8b4e8`,
was then uploaded to the same native session.

Two inspections, separated by another matching 64 MiB EFI read, returned
identical values. MSI-X was enabled, entry zero was unmasked with address
`0xfe610040` and data 464, and its pending bitmap was zero. GIC ID 464 was
enabled, edge-triggered, routed to CPU 0 at priority `0x80`, and neither pending
nor active. PHP_GRF ITS address selectors remained `0xfe65` and `0xfe67`;
PCIe MMU mode/control were `3`/`0x28`. These reads establish the programmed
state but do not locate where PCIe messages were lost. No routing register
writes were attempted.

All six component hashes, strict zero BFS allocation counters and one RNDIS
notification worker passed at session closure. No USB checksum/control timeout
errors appeared in the workload window. Recovery returned ROOBI
`4488812d-d230-491b-b6ec-2385f8110a39`; the guard disarmed and NanoKVM's boot ID
remained unchanged. `state/native-mbi-provider.json` retains the MSI failure;
`state/native-msi-inspect.json` separately records the diagnostic pass and
verified recovery. The installed SSD components were unchanged.

A subsequent read-only Linux reference in
`linux-nvme-msix/20260912T220657Z-de3cda` captured nine unmasked NVMe MSI-X
entries with address `0xfe670040` (ITS1) and event data 0 through 8. The same
64 MiB EFI read matched the reference hash and generated 513 NVMe interrupts.
The MSI-X table was unchanged across the read. `state/linux-nvme-msix.json`
records the kernel, actual Linux resources, table and interrupt counts. ITS/LPI
support is the next implementation step; this Linux observation is not a Haiku
ITS pass.
