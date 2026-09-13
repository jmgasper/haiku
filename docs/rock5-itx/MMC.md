# MMC and onboard eMMC

The onboard eMMC now passes native read-only geometry and data checks before
and after normal Haiku reboot. Three 8 MiB regions match Linux, including data
beyond 4 GiB and at the end of the device. The common MMC stack also passes
SD/eMMC writes and persistence in ARM64 QEMU. Native writes, faster speed modes
and Haiku boot from eMMC remain unqualified. MicroSD uses a different controller
and is not covered by this work. ROOBI remains on eMMC; these native tests have
not written it.

## Common driver checkpoint

Source `e6ec002c8f2b4311f80fe516a5a58c8ebd55b2b3` (`hrev60097+127`) adds
MMC CMD1 sector-addressing negotiation, PIO EXT_CSD reads, user-area capacity and
cache-state discovery, corrected native R2 CID/CSD decoding, and MMC disk
geometry. It supports four-bit legacy SDR, explicit cache-flush/status commands,
write-protection checks and rejection of SD erase commands for MMC. EXT_CSD
user areas with 512-byte sectors and nonzero SEC_COUNT are supported; older MMC
without that information remains unsupported.

Command, transfer, busy-response, clock and reset waits are bounded. A failed
controller reset prevents further commands. The change also corrects high
clock-divider bits, accumulates completion events, acknowledges exactly the
handled interrupt bits and cleans up partial initialization. Card power/clock
setup runs in the bus worker instead of the insertion interrupt handler.

The PCI/ACPI host-controller interface is now `device/v2`; the disk-facing
interface and R2 layout are unchanged. The newer ARM64 SDMA buffer is described
below. Hotplug/removal and broader error recovery are open.

At the `+127` checkpoint, ninety-eight host checks pass. Four suites exercise
production MMC decoders, disk routines, bus lifecycle and SDHCI command/PIO/DMA
routines under ASan/UBSan.
Cases include the board's CID/CSD, fields crossing R2 words, unsigned capacities,
failed mode switches, write-protected media, failed/busy flushes, missing
completions, CRC errors, short PIO, failed resets, invalid DMA vectors and masked
events on a shared IRQ. The persistence oracle rejects absent writes and damaged
guard bytes.

## QEMU evidence

QEMU 10.2.0 supplies an 8 GiB SD card and a distinct 9 GiB eMMC, each on a PCI
SDHCI controller. Both report 512-byte sectors. The test verifies initial data,
overwrites at 5 GiB, five partial-sector writes per card and a 2 MiB guard region
after every partial write. Both cards pass readback after normal reboot. After
normal shutdown, six independent host reads of the backing files match.

The emulated eMMC reports its cache disabled. Its explicit flush ioctl waits
for CMD13 ready/transfer state; the cached-card CMD6 FLUSH_CACHE branch is covered
by host fault tests, not this QEMU card. SD explicitly reports flush unsupported.
Backing-file persistence does not establish physical power-loss behavior.

The combined run also passes USB login, memory/cache/copy, service-descriptor,
transfer, PCI Ethernet and NVMe/AHCI regressions. The existing QEMU NVMe fixture
still labels raw-device cache flush untested: its `fsync` is not a drive flush.

| Evidence | Location under `/mnt/HaikuWork` |
| --- | --- |
| Image manifest | `artifacts/mmc-image/20260913T134810Z-3e7876/manifest.json` |
| Image SHA-256 | `cac9fdc4583b8bbe36bfcb74df5cb2764afe9ae281b5fe0c1ab137a8b731b0d1` |
| Full ARM64 build | `artifacts/build-20260913T134721Z.log` |
| Host checks | `artifacts/mmc-image/20260913T134810Z-3e7876/host-checks.log` |
| Combined QEMU result | `artifacts/qemu-shell/20260913T134811Z-a7e75e/result.json` |
| Card transcripts and backing files | `artifacts/qemu-shell/20260913T134811Z-a7e75e/mmc/` |
| Failed pre-fix run | `artifacts/qemu-shell/20260913T134319Z-9b8313/result.json` |

The `+126` combined run discovered correct capacities but USB RNDIS/HID control
requests timed out before remote login. It has no I/O acceptance. The driver
had claimed masked PIO/SDMA status on a shared level-triggered IRQ, preventing
the next handler from running. Filtering ownership by enabled signals is the
only production change in `+127`, which passes. This does not resolve the
separately recorded native USB outages or older SSD startup stall.

The previous `+124` image detected the emulated MMC card but published only the
SD disk (`artifacts/qemu-shell/20260913T132258Z-6a3194`). Earlier setup attempts
failed before guest execution: QEMU 9.2.4 did not allow user-created eMMC and
two bus-path spellings were rejected. The admitted path is
`pcie.0/mmc0/sd-bus`, with `mmc1` for the second card.

## Repeat the emulator tests

The system QEMU 8.2.2 is retained. Build and use the local emulator:

```sh
source tools/rock5-itx/env.sh
bash tools/rock5-itx/build-qemu-mmc.sh
python3 tools/rock5-itx/qemu_shell.py /mnt/HaikuWork/artifacts/mmc-image/20260913T134810Z-3e7876/manifest.json \
    --mmc --memory --platform --cache --memcpy --services --transfer \
    --nvme --ahci --pci-network --network-stream --power --normal
```

The QEMU 10.2.0 source archive SHA-256 is
`9e30ad1b8b9f7b4463001582d1ab297f39cfccea5d08540c0ca6d6672785883a`.
The script records the binary hash, DTC revision and host library versions in
`state/qemu-mmc-toolchain.json`; each test embeds the receipt and checks the
binary hash. Missing pinned Ubuntu 24.04 amd64 Ninja/libslirp development
packages can be extracted locally. Other compiler/development libraries come
from the workstation. The only omitted archive entry is an unused macOS X11
convenience symlink. System QEMU is not replaced.

Each guest test verifies the exact MMC bus, SDHCI, disk and helper binaries.
`--mmc` requires normal reboot/shutdown and excludes the fixed PCI inventory
test. Scratch images are newly created for every run.

The optional `--mmc-filesystem` extension adds a third disposable eMMC with a
300 MiB FAT32 partition. It also requires `mmc_test.fat_sha256` in the image
manifest and local dosfstools/mtools. An existing 16 MiB target file receives
an 8 MiB write and five partial-sector writes from a distinct source file.
After each write, the test synchronizes and unmounts the filesystem, explicitly
flushes the MMC device, then mounts read-only and checks both complete files.
Readback repeats after normal reboot. Following shutdown, the host independently
checks FAT consistency, file hashes, the partition table and adjacent guards.
The host oracle rejects absent writes and corruption outside the write ranges.
This extension is being qualified before native eMMC file writes; it does not
change the scope of the accepted read-only hardware result below.

Its first `+133` run stopped before file writes because the geometry helper's
legacy `B_GET_DEVICE_SIZE` ioctl reported raw-card capacity for the partition.
The helper now queries `B_GET_PARTITION_INFO`, validates the extent against raw
capacity and reports its exact offset, size and parent. The file test requires
all three before mounting. The retained failed run is
`artifacts/qemu-shell/20260913T153251Z-2ecc27`; native media was not changed.

## Linux and board reference

The board is ROCK 5 ITX PCB v1.12, running ROOBI / Debian 11 and Linux
`5.10.110-33-rockchip`. The read-only reference uses boot ID
`3af7cbce-3be4-47a3-a5ec-46b229dc069c`.

| Item | Observed value |
| --- | --- |
| Host | `fe2e0000.mmc`, Linux `sdhci-dwcmshc`, MMC0 |
| Card | Samsung `8GTF4R`, MID `0x15`, January 2024 |
| CID | `15010038475446345206ee534de21b00` |
| User area | 15,269,888 sectors; 7,818,182,656 bytes |
| EXT_CSD | Revision 8; 512-byte sectors; user partition selected; cache enabled |
| Linux timing | Eight-bit HS400 enhanced strobe; 200 MHz reported actual card clock; 1.8 V signaling |
| Clock-summary observation | Core/block/bus 198 MHz, AXI 300 MHz, timer 24 MHz; distinct from the `ios` report |
| Firmware FDT | `rockchip,rk3588-dwcmshc`, base `0xfe2e0000`, size 64 KiB, level-high SPI 205 / GIC INTID 237 |
| Other card hosts | `fe2c0000` and `fe2d0000`, Linux `dwmmc_rockchip`; no cards detected |

Inventory and the first-8-MiB user-area hash are retained in
`artifacts/emmc-baseline/20260913T125317Z-ec7f88`. Both 4 MiB eMMC boot areas
were hashed separately. Linux root is live and mutable; arbitrary root-region
hashes cannot be assumed to survive another Linux boot.

Documented controller/configuration registers were read through a read-only
mapping without touching command/data FIFOs. Evidence is in
`artifacts/emmc-register-reference/20260913T135309Z-33f081`, SHA-256
`960551750b02745888ea799caafa6a435f4880a170192372d5c9f5bc924eeac7`.
Capabilities are `0x226dc881` / `0x08000007`, SDHCI version field 5, vendor area
offset `0x500`, and CRU CLKSEL_CON77 `0x590`. These describe running Linux,
not an assumed firmware handoff state.

## Native driver and first trials

Sources `ae716e8c93` and `3029920dff` (`hrev60097+129` / `+130`) add the RK3588
FDT attachment and a private 512 KiB noncacheable SDMA buffer below 4 GiB. Caller buffers can be
above 4 GiB; copies occur on the CPU, and failed reads are not copied back.
An unrecoverable command/data reset disables further requests and retains the
DMA allocation unless a subsequent full reset confirms that it can be freed.

The attachment requires this explicit `sdhci` driver setting:

```text
firmware_profile rock5-itx-edk2-v1.1-emmc-legacy
read_only true
```

It admits the recorded board, controller and CRU resources, interrupt,
clock/reset providers, identity DMA buses and controller capabilities before
changing registers. Read-only access is the default, and disk geometry marks
the soldered device nonremovable. High-speed modes and enhanced strobe are
disabled. The external core/PHY source is programmed to 375 kHz for
identification and 24 MHz for legacy operation, preserving adjacent NVM bus
clock fields. The SDHCI divider remains nonzero. These are source-clock
settings, not an electrically measured card-clock rate.

One hundred and one host checks pass, including production resource admission,
clock sequencing, DMA allocation/cleanup, physical caller buffers above 4 GiB,
failed-read isolation, read-only geometry and reset after an unsupported SD
probe. All three images, `+129` through `+131`, passed the complete SD/eMMC QEMU
fixture and combined regressions with the actual ARM64 DMA allocator enabled.

The first native `+129` trial initialized the controller and allocated DMA at
`0x2e80000`, but CMD1 returned `0xffffffff` and CMD2 timed out. No MMC disk was
published, so native I/O failed its gate. Desktop, all 24 component and five
setting hashes, eight-worker memory and 51,301 copy cases passed. Read-only
register diagnostics confirmed the programmed clocks, `0x1111` pin mux values
and enabled inputs. Evidence is in
`artifacts/interactive/20260913T142828Z-9e5a06`; the image manifest is
`artifacts/mmc-image/20260913T142433Z-cfca7b/manifest.json`.

The NanoKVM guard disarmed normally, and recovery returned ROOBI boot
`7ae30d53-84f7-44c4-b0ec-65be6aa2f9b5`. Three 8 MiB Linux reference regions at
offsets zero, 5 GiB and the end of the user area remained unchanged. Unlike
`+129`, `+130` lowers the external clock during identification, following the
installed firmware's initialization.

The second native `+130` trial returned a valid OCR voltage field (`0x00ff8080`),
finished the busy period and completed CMD2. CMD3 returned `0x00400500`, including
the `ILLEGAL_COMMAND` bit. Source `cee0be9cdf` (`+131`) returns the card to idle
before MMC negotiation so an unsupported SD CMD8 cannot leave that error for
the first MMC R1 response. It also rejects an all-ones OCR.
The second trial's diagnostics, desktop and component/memory/copy checks are
in `artifacts/interactive/20260913T144852Z-70d332`; the manifest is
`artifacts/mmc-image/20260913T144450Z-48e0b5/manifest.json`. No MMC disk was
published or user-area I/O attempted. Recovery returned ROOBI boot
`a4faa8ee-0b14-4f6b-8f26-879c2a3ceb99`, and the guard disarmed normally.

## Native read-only qualification

Source `cee0be9cdfeab92e5ef8504bd7e4d7ede498791f` (`hrev60097+131`) passed
two native boots with a normal PSCI reboot between them. The driver published
`/dev/disk/mmc/0/raw`, reporting 512-byte sectors, 7,818,182,656 bytes and
read-only geometry. EXT_CSD revision 8 and 15,269,888 sectors match Linux.
The native card reported its cache disabled after firmware initialization.

On each boot, three 8 MiB regions at offsets zero, 5 GiB and 7,809,794,048
matched their Linux SHA-256 references. Total native checked payload is
50,331,648 bytes (48 MiB). The short reads measured approximately 7.8–8.3 MB/s;
these are initial integrity checks, not a throughput qualification. Linux
independently read the same regions after recovery and obtained the same hashes.
No native eMMC writes or flush ioctls were requested.

Both boots also passed all 24 component and five setting hashes, an eight-worker
64 MiB memory check and 51,301 copy cases. Tracker/Deskbar desktops were inspected.
Both Ethernet interfaces obtained DHCP with 2.5/1 Gbit/s links. This run did not
repeat Ethernet traffic qualification or mount/update the `+94` SSD installation.
The DMA allocation was below 4 GiB; CPU caller buffers above 4 GiB were tested
by the host fixture, not forced in this native run.

The expected unsupported SD CMD8 timeout precedes the successful MMC fallback.
Each initialization also printed `Remaining interrupts at end of handler: 2`
once. That is the transfer-complete bit; subsequent EXT_CSD and all data reads
completed. The timing cause has not been isolated, so this result does not
establish complete interrupt/error-recovery behavior. Neither boot recorded a
kernel panic or another failed SDHCI command.

| Evidence | Location under `/mnt/HaikuWork` |
| --- | --- |
| Image manifest | `artifacts/mmc-image/20260913T150111Z-e19d9f/manifest.json` |
| Image SHA-256 | `416103cd1c264f9c83b26df85656740c0d3e2c83304878f7c823c422a63974b7` |
| Full ARM64 build | `artifacts/build-20260913T150019Z.log` |
| 101 host checks | `artifacts/mmc-image/20260913T150111Z-e19d9f/host-checks.log` |
| Combined QEMU result | `artifacts/qemu-shell/20260913T150112Z-52ab0f/result.json` |
| Native session and reviewed result | `artifacts/interactive/20260913T150450Z-7f2149/qualification.json` |
| Native read transcripts | Same directory: `shell-20260913T150930Z-e10882.txt`, `shell-20260913T151620Z-5630cb.txt` |
| Post-recovery Linux reads | `artifacts/emmc-read-reference/20260913T151816Z-329dc0/result.json` |
| Controller guard | `artifacts/controller-guarded-native/20260913T150450Z-866339/watchdog-result.json` |

Serial capture completed without transport errors. Recovery returned ROOBI boot
`e03fe45c-89ea-4529-94e9-95f820a9070d`; NanoKVM stayed up and its guard disarmed.
`state/native-mmc-read-only.json` indexes the qualification and both preceding
failures. The existing complete user-area, boot-area and SPI backup files and
the tested restoration image were rehashed successfully before planning writes.

## Native work remaining

Extend the bounded read result to guarded scratch-file writes and explicit
flush/reboot persistence while preserving ROOBI and its tested recovery route.
Longer mixed I/O, native caller buffers above 4 GiB, speed negotiation/tuning,
power-cycle integrity, error recovery and Haiku boot from eMMC remain open.

The TRM specifies a 32-bit eMMC AXI address interface. Core clock selection uses
CRU `0xfd7c0000 + 0x434`, with high-word write masks. Other clocks in that register
must be preserved. Linux/EDK2 reference behavior does not establish Haiku support.

References: [QEMU eMMC model](https://www.qemu.org/docs/master/system/devices/emmc.html),
[Linux DWC MSHC implementation](https://github.com/torvalds/linux/blob/v6.12/drivers/mmc/host/sdhci-of-dwcmshc.c),
[FDT binding](https://github.com/torvalds/linux/blob/v6.12/Documentation/devicetree/bindings/mmc/snps,dwcmshc-sdhci.yaml),
[MMC protocol definitions](https://github.com/torvalds/linux/blob/v6.12/include/linux/mmc/mmc.h).
The [installed EDK2 eMMC driver](https://github.com/edk2-porting/edk2-rk3588/blob/6a682c0ef3ed74feb8b0d98f1c2aa771ddfbae18/edk2-rockchip/Silicon/Rockchip/Drivers/DwcSdhciDxe/DwcSdhciDxe.c)
provides the firmware clock and initialization reference.
Local RK3588 TRM v1.0 Part 1 clock/interrupt chapters and Part 2 Chapter 4 are
retained in `artifacts/reference`; installed EDK2 v1.1 source is retained in
`artifacts/firmware-source/edk2-rk3588-v1.1`.
