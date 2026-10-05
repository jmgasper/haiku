# Integrating the qualified board drivers on the SSD

The Samsung 950 Pro now runs `hrev60097+156`, including the PCIe training fix.
Two installed boot cycles passed the component, storage, network and recovery
checks. The first SSD boot encountered the original SATA training condition:
the driver waited 1,577 us, then initialized AHCI and all four direct ports.
The second boot found the link ready. This qualifies the installed update and
that specific transition; sustained boot reliability and native SATA disk I/O
remain open.

The [earlier +148 integration](SSD-INTEGRATION-148.md) preserves its incomplete
first boot and subsequent accepted checks. [PCIe training](PCIE-TRAINING.md)
records the diagnostic, bounded wait and source-image qualification.

## Candidate and installation

| Item | Value |
| --- | --- |
| Driver source | `26a109b5ef2d7138f4f512abb580e705dbeff423` |
| Source image | `artifacts/pcie-training-image/20260913T224921Z-a4d43d/manifest.json` |
| Image SHA-256 | `fabc17620f00a19bca06ff7b1c2715d2fbaa24755cb9346d8b475f673e70563b` |
| ARM64 build | `artifacts/build-20260913T224829Z.log` |
| Integration plan | `state/pcie-training-installed-update-plan.json` |
| Integration evidence | `artifacts/pcie-training-installed-update/20260914T023126Z-d7825c` |

Local paths are beneath `/mnt/HaikuWork`. The source passed all 111 host checks,
the combined ARM64/QEMU regression and two complete native USB boot checks
before SSD integration. Only the PCI host has changed executable code relative
to the diagnostic candidate; the five settings retain the qualified profile.

The standard Installer updated the existing 238 GiB BFS volume. Three generated
Haiku packages changed from `+148` to `+156`; the other eight package hashes
match. Nineteen board, helper and startup files were staged, hashed and renamed.
The previous three Haiku packages and all 22 copied/retained files were backed
up and checked. Both NIC registration links, the startup observer and the PTY
probe were retained. No raw NVMe scratch writes were used.

The existing EFI loader remains at SHA-256
`31d8f11cef5a998ad2ef0a29608397dc1d1ce51cb8cfe4d6aea7eadfd1d9dc75`.
One-time EFI selections booted the SSD while preserving the recovery image and
boot order. Both boots used this loader to start the new kernel.

## Accepted checks

The emulated upgrade passed before/after checks, full readback of its retained
5,377,097,728-byte file, filesystem checks and normal reboot. The updated image
then booted directly as NVMe twice, with memory, cache, terminal and transfer
checks. Two emulated SATA disks passed reads, writes, explicit flushes and
independent host readback after shutdown in the same run.

On the physical SSD, both boots checked all 25 components, 11 packages,
configuration files, the 64 MiB/eight-worker memory test, 51,301 copy cases and
the retained PTY regression. Two guarded 2 GiB regions in existing regular
files passed eight-worker pattern verification, SHA-256 and all four guard
hashes. BFS reported zero missing, duplicate or unreferenced blocks. NVMe used
ITS1 MSI-X vector 8192 on CPU0, with interrupt delivery recorded during I/O.

Both RTL8125 ports obtained DHCP at negotiated 2.5/1 Gbit/s. Each boot passed
four simultaneous IPv4 streams, one send and one receive per port, each of
32 MiB plus seven bytes. Complete TCP sequence coverage and both MAC addresses
proved the physical paths. Data, interface error/drop and capture-drop checks
passed; temporary workstation addresses and capture processes were removed.

| SSD boot | Port 0 receive | Port 0 send | Port 1 receive | Port 1 send |
| --- | ---: | ---: | ---: | ---: |
| 1 | 327.4 | 207.0 | 347.0 | 208.5 |
| 2 | 237.1 | 170.3 | 232.6 | 159.4 |

Rates are Mbit/s from the ROCK's perspective in short application-level runs.
They remain below Linux and do not establish maximum throughput.

Both boots initialized four direct SATA ports with INTx IRQ 287 and checked
read-only eMMC files and reference regions. eMMC retained eight-bit legacy
SDR, its verified 64 MiB cache and CPU buffers above 4 GiB with private DMA32
buffers. Linux independently confirmed the complete 300 MiB FAT partition,
both fixture files, FAT consistency and all three reference regions unchanged.
The partition SHA-256 remains
`7eabc57f93999c0342176a487872fa5fa2bf80ec133532627b426d0f909f8da4`.

| Gate | Passing evidence beneath `/mnt/HaikuWork` |
| --- | --- |
| Source QEMU | `artifacts/qemu-shell/20260913T225022Z-a67e77/result.json` |
| Emulated Installer and reboot | `artifacts/qemu-shell/20260914T023351Z-40f879/installer-result.json` |
| Updated emulated SSD and SATA disks | `artifacts/qemu-shell/20260914T030234Z-aa6324/updated-nvme-boot-result.json` |
| Physical installation | `artifacts/interactive/20260914T030908Z-20ce9b/result.json` |
| First physical SSD cycle | `artifacts/pcie-training-installed-update/20260914T023126Z-d7825c/native-installed-boot-1.json` |
| Second physical SSD cycle | `artifacts/pcie-training-installed-update/20260914T023126Z-d7825c/native-installed-boot-2.json` |
| Linux FAT readback | `artifacts/emmc-file-readback/20260914T033426Z-b7f4ec/result.json` |
| Linux reference regions | `artifacts/emmc-read-reference/20260914T033427Z-0688c3/result.json` |

Both normal Haiku reboots returned to Linux. Serial capture completed cleanly,
the guards disarmed and NanoKVM kept its boot ID. The integration directory's
`final-qualification.json` and `state/native-pcie-training-installed-update.json`
index the evidence; `state/installed-current.json` identifies this baseline.

Earlier startup stalls and USB control outages, sustained and fault-recovery
tests, Ethernet performance, faster eMMC modes and the remaining board hardware
remain separate work. No physical SATA disk is identified. The sealed emulated
SSD is a regression fixture and must not be written onto the physical drive.
