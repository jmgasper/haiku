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
MBIS support, Group 1, and an inactive, disabled test interrupt.

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
NVMe, USB and reboot gates. Native message delivery and cleanup remain pending.
