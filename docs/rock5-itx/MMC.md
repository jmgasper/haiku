# MMC and onboard eMMC

The common MMC stack exposes a sector-addressed eMMC user area and passes
SD/eMMC I/O tests in ARM64 QEMU. **Native RK3588 eMMC attachment and I/O remain
untested.** MicroSD uses a different controller and is not covered by this work.
ROOBI remains on eMMC; these tests have not written it.

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
interface and R2 layout are unchanged. The SDMA data path still requires native
ARM64 noncoherent DMA work. Hotplug/removal and broader error recovery are open.

Ninety-eight host checks pass. Four suites exercise production MMC decoders,
disk routines, bus lifecycle and SDHCI command/PIO/DMA routines under ASan/UBSan.
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

## Native work remaining

Add the explicit RK3588 FDT attachment and validate its resource/clock profile
before register writes. Establish a legacy clock and DLL-bypass configuration,
read EXT_CSD and compare user-area reads against Linux while preserving ROOBI.
Native DMA needs 32-bit address admission and explicit cache ownership or a
private noncacheable bounce buffer. Scratch-file writes, flush/reboot persistence,
speed negotiation/tuning, error recovery and native boot follow read-only
qualification and a complete recovery backup.

The TRM specifies a 32-bit eMMC AXI address interface. Core clock selection uses
CRU `0xfd7c0000 + 0x434`, with high-word write masks. Other clocks in that register
must be preserved. Linux/EDK2 reference behavior does not establish Haiku support.

References: [QEMU eMMC model](https://www.qemu.org/docs/master/system/devices/emmc.html),
[Linux DWC MSHC implementation](https://github.com/torvalds/linux/blob/v6.12/drivers/mmc/host/sdhci-of-dwcmshc.c),
[FDT binding](https://github.com/torvalds/linux/blob/v6.12/Documentation/devicetree/bindings/mmc/snps,dwcmshc-sdhci.yaml),
[MMC protocol definitions](https://github.com/torvalds/linux/blob/v6.12/include/linux/mmc/mmc.h).
Local RK3588 TRM v1.0 Part 1 clock/interrupt chapters and Part 2 Chapter 4 are
retained in `artifacts/reference`; installed EDK2 v1.1 source is retained in
`artifacts/firmware-source/edk2-rk3588-v1.1`.
