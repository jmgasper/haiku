# SATA / ASM1164 bring-up

The board has an ASMedia ASM1164 AHCI controller (`1b21:1164`, revision 02),
at Linux `0001:11:00.0`. No SATA disk was present in the 2026-09-13 Linux
baseline. Controller attachment and disk I/O remain separate acceptance gates.
The Samsung 950 Pro is an NVMe device and is not a SATA test fixture.

The `+148` SSD integration includes this driver. Two accepted installed boots
initialized all four ports, but an earlier boot rejected the initial PCIe
firmware profile and missed AHCI attachment. That intermittent startup failure
remains unresolved; [SSD-INTEGRATION.md](SSD-INTEGRATION.md) records all three
attempts and the additional emulated SSD/SATA regression.

The `+154` USB diagnostic reproduced this failure. Segment 1's root snapshot
reported link status `0xb823`: the data link was active at x2 and speed encoding
3, but Link Training (`0x0800`) was still set. Every other firmware-profile
condition matched. The root passed validation later in that same boot, but
AHCI did not attach. A normal reboot of the unchanged image initialized all
four ports. This identifies the rejected condition; it does not yet fix the
startup race. The two boots and their integrity checks are recorded in
[PCIe training diagnostics](PCIE-TRAINING.md).

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

## Driver changes

The native `+123` trial reached the remote shell using IRQ 287, but reported
CAP.NP = 23 and PI = `0x00ffff0f`: four direct ports and 16 virtual ports
at indices 8..23. Probing all of them delayed boot. This did not pass the
four-direct-port readiness gate; the complete trial is retained at
`/mnt/HaikuWork/artifacts/interactive/20260913T121042Z-123d3e` and
`state/native-ahci-unmasked.json`. Recovery succeeded without serial errors.

The optional `asm1164_direct_ports_only true` setting in `ahci` restricts
this device's software port map to bits 0..3. It does not rewrite PI or
change other controllers. The default retains all reported ports, because
virtual ports can represent actual disks behind a port multiplier. Linux
added an explicit mask option after reverting an unconditional restriction
for related ASMedia controllers; see
[the Linux port-mask change](https://git.ti.com/cgit/ti-linux-kernel/ti-linux-kernel/commit/drivers/ata?h=v5.10.161&id=24cfd86433c920188ac3f02df8aba6bc4c792f4b).
Port-multiplier hardware is not present in this lab and remains untested.

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
The QEMU fixture uses two independently seeded disposable 512n/512e disks, explicit
drive-cache flushes, guard-checked partial-sector writes, reboot readback,
and independent backing-file hashes after shutdown. These checks passed as
described below; native SATA disk I/O still needs a physical fixture.

## Accepted controller checkpoint

Source `5f8d9b0ba429a4e968b33bf02bf950e45edd3b2e` (`hrev60097+124`)
passed all 92 host checks and the full ARM64 build. The immutable USB image
has SHA-256
`4d225c32395d8cb3df205d5bb03b2e11423840c38ddeacf5f4a9cc2fe7b25c5e`.
Its manifest pins twenty components and four settings, including the explicit
ASM1164 direct-port selection, firmware PCI profile and ITS1 NVMe setting.

QEMU passed two-disk AHCI I/O before and after normal reboot. The disks have
512-byte logical sectors, with 512-byte and 4096-byte physical sectors
respectively. Both passed high-offset writes, five partial-sector write sizes
from 1 byte through 1,048,579 bytes, surrounding guard checks and explicit
`B_FLUSH_DRIVE_CACHE`. After shutdown, six independent backing-file hashes
confirmed the two heads, high-offset regions and final guard regions. The
existing memory, instruction-cache, copy, service, USB/network, IPv4/IPv6,
route-query, NVMe and power checks also passed. AHCI used IRQ 36 in this
emulated topology; this does not qualify the ROCK's interrupt delivery or
noncoherent DMA with a real disk.

The earlier `+122` QEMU fixture did not boot: QEMU 8.2's `ide-hd` rejects a
4096-byte logical sector. Its error artifacts are retained. The accepted
fixture uses 512n/512e disks, and **4Kn I/O remains untested**.

Native session `interactive/20260913T122354Z-c3acae` reached the desktop and
authenticated USB shell twice, separated by a normal software reboot. Both
boots admitted INTx IRQ 287, selected PI bits 0..3 from `0x00ffff0f`, and
initialized only those four direct ports. All component/settings hashes, a
64 MiB eight-worker memory check and 51,301 actual libroot copy cases passed
on each boot. No SATA disk was attached; these observations establish
controller initialization and reboot, not disk discovery or data transfer.

The same native session checked 268,435,512 bytes of concurrent traffic over
both Ethernet ports: IPv4 on the first boot and IPv6 after reboot. Complete
captures contain 90,505 frames with no capture drops or interface errors.
IPv6 also passed discovery in both directions, checked echoes and selection
of the second port's specific route while a default existed through the
first port. The links remained at 2.5 and 1 Gbit/s. Short transfer rates ranged
from 143 to 396 Mbit/s, so Linux throughput parity remains open. One earlier
peer fixture expired before its guest command was submitted; its failure and
successful cleanup are retained separately and do not count as a native run.

The Samsung NVMe installation was not mounted or updated and remains at
`hrev60097+94`. Serial capture completed without transport errors, ROOBI
returned with a new boot ID, NanoKVM retained its boot ID and its watchdog
disarmed. The temporary workstation network configuration was removed.
Acceptance evidence is indexed by:

- `state/ahci-checkpoint.json` and `state/native-ahci-controller.json`
- `artifacts/ahci-image/20260913T121921Z-a6c4e1/manifest.json`
- `artifacts/qemu-shell/20260913T121922Z-cff830/ahci-qualification.json`
- `artifacts/interactive/20260913T122354Z-c3acae/qualification.json`

## Remaining SATA acceptance

An identified disposable SATA disk is needed for initial native read/write,
DMA coherency, actual interrupt delivery, explicit flush and independent Linux
readback. Then test each of the four physical ports, simultaneous disks,
filesystem integrity, TRIM where supported, link/error recovery and sustained
load against a Linux reference. High-memory DMA, physical 4Kn disks and port
multipliers need their own fixtures and evidence. The controller checkpoint
does not accept the full SATA roadmap row.

## References

Reference: [Intel AHCI 1.3.1 specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf),
especially command/PRD structures, transfer completion and port shutdown.
The local PDF SHA-256 is
`3bbd4b7fc7dbef60948f3e56575d860ffc02a4d3571a20cd757aa2b22fa8e56a`.
Linux baseline and retained firmware evidence:

- `/mnt/HaikuWork/artifacts/sata-baseline/20260913T113318Z-f46161`
- `/mnt/HaikuWork/artifacts/firmware-trials/20260911T071501Z-93f34c/efi-diagnostic/firmware.dts`
- `/mnt/HaikuWork/artifacts/reference/rk3588-trm-v1.0-part1-20220309.txt`
- `/mnt/HaikuWork/artifacts/reference/rk3588-trm-v1.0-part2-20220309.txt`
