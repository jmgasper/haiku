# Integrating the qualified board drivers on the SSD

The physical Samsung 950 Pro installation has been updated from `hrev60097+94`
to the previously built and qualified `+148` USB components. Two repeat SSD
boots pass the bounded integration checks; an earlier attempt missed SATA
initialization and remains unresolved. The changes include both RTL8125 interfaces,
ASM1164 SATA discovery, eight-bit eMMC and the DMA fixes described in
[ETHERNET.md](ETHERNET.md), [SATA.md](SATA.md) and [MMC.md](MMC.md).

## Pinned candidate and scope

| Item | Value |
| --- | --- |
| Driver source | `bc21c9ef581ef034a4f79ef2185b4e101ac3772b` |
| Source image | `artifacts/mmc-cache-image/20260913T192431Z-c574d2/manifest.json` |
| Image SHA-256 | `81a39a804b835f514c13f00805e475551addf9a078c4571493c79f3e17f71b31` |
| ARM64 build | `artifacts/build-20260913T192200Z.log` |
| Source-image combined QEMU qualification | `artifacts/qemu-shell/20260913T192454Z-9f4d22/result.json` |
| Source-image native qualification | `artifacts/interactive/20260913T192959Z-778685/qualification.json` |
| Integration plan | `state/integrated-installed-update-plan.json` |
| Integration evidence directory | `artifacts/integrated-installed-update/20260913T205321Z-25da07` |

Paths in this document are beneath `/mnt/HaikuWork`. The candidate contains
eleven packages. Three generated Haiku packages change from `+94` to `+148`;
the other eight names and hashes match. Local snapshots of the three generated
packages match the native USB inventory and independent extraction from the
read-only candidate image.

The installed profile keeps the candidate's exact tested settings: onboard
PCIe with legacy interrupts, ITS1 for NVMe, four direct ASM1164 ports, and
read-only eMMC with its verified cache enabled and CPU buffers forced above
4 GiB. The eMMC controller retains private DMA32 buffers and legacy clocks.
These settings establish a reproducible development baseline; sustained
performance and faster eMMC modes remain separate work.

The existing EFI loader is retained with SHA-256
`31d8f11cef5a998ad2ef0a29608397dc1d1ce51cb8cfe4d6aea7eadfd1d9dc75`.
The bootloader source and boot argument headers have no changes between the
qualified installed baseline and the candidate. Direct installed-system boots
must verify that this loader starts the updated kernel.

## Update procedure

The standard Installer replaces the system packages on the existing BFS
volume. The copy engine preserves the settings directory, so the procedure
explicitly stages, hashes and renames nineteen configuration/helper files:
five board/package settings, two non-packaged NIC drivers, ten test helpers
and two boot/environment scripts. It verifies both NIC registration symlinks.
The existing startup observer and PTY probe are retained and checked. The
source USB image itself does not contain that observer.

Before copying, checks identify the SSD partition, old packages, settings,
EFI loader and test files. The three old Haiku packages and six installed
settings/observer files are backed up to a named directory under the target's
`home/rock5-lab`. The new packages, copies and backups are checked after
installation. Filesystem checks require zero missing, duplicate or unreferenced
blocks; command exit status alone is insufficient. Sync and clean unmounts
precede reboot and readback.

The QEMU target has the same 512 MiB EFI and 238 GiB BFS layout as the physical
SSD. Its retained test file is 5,377,097,728 bytes, with full SHA-256
`4ae4c962b39cf205cedf938470f12f16294173e3167477b056369b9dafca3143`.
The physical installation has two existing guarded 2 GiB test regions in
regular files. The old raw NVMe scratch ranges overlap BFS and must not be
used.

## Qualification

| Gate | Evidence and result |
| --- | --- |
| QEMU old-installation checks and backup | Pass; `qemu_precheck` in the integration plan |
| QEMU Installer completion | Inspected `artifacts/qemu-shell/20260913T205642Z-1be626/installer-20260913T210549Z-ecde1d.ppm` |
| QEMU copies, packages, full file and filesystems | Pass; `qemu_postcheck` in the integration plan |
| QEMU normal reboot, fresh full readback and shutdown | Pass; `artifacts/qemu-shell/20260913T205642Z-1be626/installer-result.json` |
| QEMU boot from updated SSD, then normal reboot | Pass; `artifacts/qemu-shell/20260913T211820Z-6c4209/updated-nvme-boot-result.json` |
| Physical installation and recovery | Pass; `interactive/20260913T212448Z-578557`, native pre/post checks and recovery receipt in the plan |
| Updated QEMU SSD with two SATA disks | Pass; `qemu-shell/20260913T215608Z-d025a1/updated-nvme-ahci-result.json`, including I/O, explicit flush, reboot and independent shutdown readback |
| Two accepted physical SSD boots | Pass; `interactive/20260913T215425Z-054704` and `interactive/20260913T220416Z-5effad`; the earlier incomplete attempt is retained below |
| Independent Linux eMMC integrity check | Pass; `emmc-file-readback/20260913T221203Z-68d5a7` and `emmc-read-reference/20260913T221203Z-6b7da5` |

The sealed emulated disk's SHA-256 is
`082ed2c13a8517e2f843bfead75143273a79bc7468bde74c4b3c91f04c2fd576`.
Its manifest is `qemu-installed-manifest.json` in the integration evidence
directory. It is an emulated regression fixture and must not be written onto
the physical SSD.

The native update used the hardware session lock and recovery guard. Subsequent
one-time EFI requests selected the SSD while retaining the ROOBI recovery image
and boot order. Both accepted installed boots verified the updated components,
packages, regular-file data, startup observer, NVMe interrupts, both Ethernet
links, SATA discovery and read-only eMMC references/files. Each normal reboot
returned to Linux and its guard disarmed. Desktop captures were inspected.
The earlier startup stall, intermittent USB control failures and sustained
acceptance remain open.

The first updated SSD boot, `interactive/20260913T214353Z-c1483f`, passed its
component/package, large-file, network, eMMC and normal-reboot scripts. It did
not pass the integration gate: the SATA host initially failed the PCIe firmware
profile check, later appeared in PCI discovery, and never attached AHCI. The
next boot, with no intervening file or setting changes, initialized AHCI and
all four direct ports. The first log lacks the rejected register values, so
the cause remains unisolated. `native-first-attempt-incomplete.json` in the
integration directory retains that failure; later accepted sessions must not
be presented as a fix or evidence of reliable SATA startup.

## Accepted installed-system checks

The two accepted boots started all eight CPUs, passed the 64 MiB/eight-worker
memory check, all 51,301 memory-copy cases and the retained PTY regression.
Eleven package hashes and the component/configuration hashes match the plan.
Both guarded 2 GiB NVMe file regions pass eight-worker pattern verification,
independent SHA-256 and all four 8 MiB guard checks. BFS reports zero missing,
duplicate or unreferenced blocks. ITS1 supplies Samsung DeviceID `0x100` with
one MSI-X vector, 8192, on CPU0; each boot logs interrupt delivery through
count 32,768. This does not qualify multi-vector or other-device ITS operation.

Both Ethernet ports obtained DHCP with negotiated links of 2.5 and 1 Gbit/s.
Each accepted boot ran four simultaneous IPv4 streams: one send and one receive
per physical port, each transferring 32 MiB plus seven bytes. The eight streams
verified 268,435,512 payload bytes across 130,717 captured frames. Full TCP
sequence coverage and MAC addresses establish both physical paths; all four
streams overlap in each run, and interface errors/drops and capture drops are
zero. Temporary workstation addresses and capture processes were removed.

| Accepted SSD boot | Port 0 receive | Port 0 send | Port 1 receive | Port 1 send |
| --- | ---: | ---: | ---: | ---: |
| First | 313.2 | 149.6 | 268.7 | 143.5 |
| Second | 328.8 | 204.5 | 291.7 | 207.6 |

Rates are Mbit/s from the ROCK's perspective, including pattern generation,
verification and acknowledgment. These short measurements vary and remain
below Linux. Captures are in `integrated-network/20260913T215929Z-6748f4` and
`integrated-network/20260913T220732Z-c3cb47`. IPv6's earlier USB qualification is
recorded separately in [ETHERNET.md](ETHERNET.md).

Each accepted boot initialized the four direct ASM1164 ports with INTx IRQ 287.
There is still no physical SATA disk. The installed eMMC profile reports
eight-bit legacy SDR, verified 65,536 KiB device cache, read-only geometry and
CPU read buffers above 4 GiB with private DMA32 buffers. Native file and raw
reference reads pass on both boots. Linux subsequently verified the full
300 MiB FAT partition unchanged at SHA-256
`7eabc57f93999c0342176a487872fa5fa2bf80ec133532627b426d0f909f8da4`,
both retained file hashes, FAT consistency and all three 8 MiB raw references.

Final recovery boot ID is `a5890ee2-35c4-4cf7-ab5a-4bb1409635aa`; NanoKVM kept
its boot ID throughout. `state/native-integrated-installed-update.json` and
the integration directory's `final-qualification.json` index the evidence and
limits. `state/installed-current.json` points to this development baseline.
