# SATA / ASM1164 bring-up

The board has an ASMedia ASM1164 AHCI controller (`1b21:1164`, revision 02),
at Linux `0001:11:00.0`. No SATA disk was present in the 2026-09-13 Linux
baseline. Controller attachment and disk I/O remain separate acceptance gates.
The Samsung 950 Pro is an NVMe device and is not a SATA test fixture.

## Firmware and interrupt contract

The retained EDK2 v1.1 device tree identifies segment 1, APB `0xfe160000`
with size `0x10000`, and the root's named `legacy` interrupt
`<0 255 4 0>`. This decodes to GIC INTID **287**, level high. RK3588 TRM
Part 1 independently lists `irq_pcie30x2_legacy` at 287 and the PCIe3_2L
APB at that base. Linux's observed IRQ 164 is an ITS-MSI allocation and
must not be used as the native legacy vector.

The existing firmware profile validates the exact EDK2 endpoint/config/BAR
layout before binding. Linux's BAR assignments differ and are not copied.
The shared PCI INTx provider masks all receive pins until the AHCI handler
exists, then enables only INTA. It uses TRM Part 2's documented hiword mask
register at `0x1c`; the reserved `0x194` register is not accessed. The stale
child interrupt node's edge flag is not used. No PHY, clock, reset, or
board-revision-dependent electrical changes are part of this step.

## Driver changes under qualification

AHCI uses the optional 32-bit PCI interrupt interface, with no truncated-line
fallback after a platform provider fails. MSI allocation, handler installation,
enabling, and teardown have explicit ownership, including partial-init failure.

ARM64 descriptors and a private 128 KiB payload per port use Normal
Non-cacheable memory. Cached allocation zeroing is evicted before changing
the private mapping. Full barriers order descriptor publication, command
issue, completion, and payload readback. The request semaphore owns the
payload until readback completes. Caller-owned physical pages keep their
existing cache attributes; bounded copies join scatter/gather spans. The
SCSI request limit and TRIM payload limit respect the private buffer, and
allocation respects controllers without 64-bit DMA addressing. Failed PRD
construction never issues a command. An unsuccessful port stop disables
future submissions and retains memory that the controller might still own.

Host tests exercise the actual production submission/copy routines and
controller lifecycle with injected errors. They cover high physical addresses,
buffer edges, partial/odd-sized payloads, malformed scatter/gather spans,
overreported completions, timeouts, failed resets, and partial initialization.
The QEMU fixture uses two independently seeded disposable disks, explicit
drive-cache flushes, guard-checked partial-sector writes, reboot readback,
and independent backing-file hashes after shutdown. QEMU and native results
will be recorded after their respective gates run; these changes alone do
not establish working native SATA disk I/O.

Reference: [Intel AHCI 1.3.1 specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf),
especially command/PRD structures, transfer completion and port shutdown.
The local PDF SHA-256 is
`3bbd4b7fc7dbef60948f3e56575d860ffc02a4d3571a20cd757aa2b22fa8e56a`.
Linux baseline and retained firmware evidence:

- `/mnt/HaikuWork/artifacts/sata-baseline/20260913T113318Z-f46161`
- `/mnt/HaikuWork/artifacts/firmware-trials/20260911T071501Z-93f34c/efi-diagnostic/firmware.dts`
- `/mnt/HaikuWork/artifacts/reference/rk3588-trm-v1.0-part1-20220309.txt`
- `/mnt/HaikuWork/artifacts/reference/rk3588-trm-v1.0-part2-20220309.txt`
