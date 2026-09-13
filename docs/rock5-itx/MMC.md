# MMC and onboard eMMC

The onboard eMMC now passes verified eight-bit legacy SDR selection, bounded
file writes, explicit flush and persistence across normal reboot in `+140`.
The earlier `+136` four-bit image also passes orderly shutdown/startup. Both
have independent Linux file, filesystem and reference-region checks.
Eight-bit shutdown/startup, high-memory callers, native cached-card flush,
power-loss integrity, faster clocks and Haiku boot from eMMC remain unqualified.
MicroSD uses a different controller and is not covered
by this work.
ROOBI remains on its eMMC root partition; the write fixture uses the separately
backed-up, previously empty 300 MiB FAT partition.

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
The extension passes with `+136`, followed by the native file qualification
below. The earlier `+131` read-only result remains a separate checkpoint.

Its first `+133` run stopped before file writes because the geometry helper's
legacy `B_GET_DEVICE_SIZE` ioctl reported raw-card capacity for the partition.
The helper now queries `B_GET_PARTITION_INFO`, validates the extent against raw
capacity and reports its exact offset, size and parent. The file test requires
all three before mounting. The retained failed run is
`artifacts/qemu-shell/20260913T153251Z-2ecc27`; native media was not changed.

The `+134` repeat also stopped before file writes at the stricter extent check
(`artifacts/qemu-shell/20260913T153850Z-4fc16e`). Source inspection found that
`KPartition::AddChild()` published the initial zero logical block size before
the partition scanner assigned it. New children now inherit the parent's
logical block size before publication, while preserving an explicitly supplied
nonzero value. The existing extent and block-size checks remain required.

The `+135` run then passed partition metadata, all six file writes, explicit
flushes and fresh-mount hashes. After reboot, the two emulated MMC devices
exchanged device numbers, and the fixture's old path-stability assumption
stopped the test. That run is retained in
`artifacts/qemu-shell/20260913T154520Z-a75388`. The fixture now rediscovers each
uniquely sized card on every boot and checks its known data; missing or
ambiguous geometry still fails. Both observed paths are recorded. This change
does not establish stable device numbering.

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

At the `+131` checkpoint, 101 host checks pass, including production resource admission,
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

## Native FAT file writes and reboot persistence

Source `921b671c8e616ce7185b8a8050350d383a1a5dad` (`hrev60097+136`) passes
native file I/O on the existing eMMC FAT partition. Linux first verified that
`mmcblk0p2` was unmounted and empty, saved all 314,572,800 bytes, and checked
the backup's hash and FAT consistency. The complete recovery images also remain
available. Linux then created an 8 MiB source file and a distinct 16 MiB target
under `HAIKUTST`; their deterministic patterns are shared with the QEMU fixture.

Before mounting, Haiku verifies the raw capacity and the partition's exact
32 MiB offset, 300 MiB length, 512-byte logical blocks and parent device.
An 8 MiB overwrite and five writes at odd byte offsets modify the existing target
file. After each operation, the script synchronizes, unmounts, issues
`B_FLUSH_DRIVE_CACHE`, mounts read-only and hashes both complete files. All six
operations pass, including the bytes outside each write. The final source and
target hashes pass again after normal PSCI reboot. No native raw-device write
commands were used; FAT and Tracker also updated filesystem metadata, including
empty `RECYCLED/_BEOS_` directories.

The checked file payload is 192 MiB, with 9,572,871 bytes requested in file
overwrites. Another 48 MiB of raw reference reads at zero, 5 GiB and the end
of the device match the Linux baseline across both boots. Both boots also pass
all 25 component and five setting hashes, memory/copy checks, inspected desktops
and DHCP links at 2.5/1 Gbit/s. The SSD was not mounted or updated and remains
at `+94`; this run did not repeat Ethernet traffic qualification.

After recovery, Linux independently copied the unmounted FAT partition. Host
`fsck.fat -n` passes, and mtools extracts both files with the expected sizes
and SHA-256 hashes. Linux also confirms the three reference regions remain
unchanged. Recovery returned boot ID `d0ba2e20-9591-432c-9bd3-d493fbd682d9`.
Serial capture completed without transport errors and the NanoKVM guard disarmed.
The final data is retained for subsequent persistence checks.

The native card reported cache disabled on both boots, so the explicit flush
ioctl exercised the ready/transfer-state check. Native cached-card CMD6 flush
and power-loss durability remain untested. The 24 MHz external source and
nonzero host divider remain the conservative legacy setup; card-clock frequency
and throughput are not qualified by this file test. Caller buffers above 4 GiB
were not forced. Two transfer-complete diagnostics appeared during each card
initialization; no additional failed SDHCI command or kernel panic was recorded.
Their timing cause remains unisolated.

All 104 host checks, the ARM64 build and the combined QEMU suite pass. The QEMU
filesystem result includes all six writes, fresh mounts, normal reboot,
independent post-shutdown file hashes, FAT consistency and surrounding guards.
The fixture records the actual card paths on each boot; this run also exercised
the observed device renumbering. The three preceding fixture failures remain
preserved above.

| Evidence | Location under `/mnt/HaikuWork` |
| --- | --- |
| Image manifest | `artifacts/mmc-filesystem-image/20260913T155221Z-9d563a/manifest.json` |
| Image SHA-256 | `bf44d09ef1fe46f5e626df4f04b73d3d026f21fdda9a4fe5f15581d813060d4f` |
| Full ARM64 build | `artifacts/build-20260913T155206Z.log` |
| 104 host checks | `artifacts/mmc-filesystem-image/20260913T155221Z-9d563a/host-checks.log` |
| Combined QEMU result | `artifacts/qemu-shell/20260913T155222Z-8dbe3a/result.json` |
| Native qualification | `artifacts/interactive/20260913T155758Z-aeeb8f/qualification.json` |
| Native file transcripts | Same directory: `shell-20260913T160341Z-21fcc7.txt`, `shell-20260913T160801Z-8a411d.txt` |
| Original FAT backup and seeded fixture | `artifacts/emmc-write-fixture/20260913T152325Z-4c2a87/` |
| Independent Linux file/FS check | `artifacts/emmc-file-readback/20260913T161051Z-32c366/result.json` |
| Post-recovery reference reads | `artifacts/emmc-read-reference/20260913T161101Z-06b516/result.json` |

`state/native-mmc-filesystem.json` indexes the complete result. The ordinary
firmware profile still defaults to read-only access. This private trial explicitly
sets `read_only false` for its controlled file workload.

## Native orderly shutdown and startup

The unchanged `+136` image also passes a separate shutdown/startup trial in
`artifacts/interactive/20260913T163033Z-bde4d4`. Its existing build, 104 host
checks and combined QEMU result above apply to the same image SHA-256.
The retained 16 MiB target file first matched the previous final hash. Haiku
then overwrote 4 MiB at file offset 8 MiB with a different source pattern,
synchronized, unmounted and issued an explicit device flush. A fresh read-only
mount verified both complete files before normal `shutdown`.

UART recorded `PSCI: requesting system off`. More than 107 seconds after
observing that marker, no new Haiku boot had occurred and NanoKVM's USB gadget
reported `not attached`. NanoKVM SSH remained reachable with the same boot ID.
HDMI capture timed out while off; no off-state frame was obtained. A single
800 ms power-button pulse then started the same selected Haiku image. There
was no intervening recovery boot or reset command between the two Haiku boots.

After startup, the source and changed target hashes pass again. Total checked
native file content is 72 MiB, plus 48 MiB of unchanged raw reference regions
across the two boots. Both boots pass the 25 component and five setting hashes,
memory/copy checks, inspected Tracker/Deskbar desktops and DHCP links at
2.5/1 Gbit/s. This run did not mount/update the SSD or repeat network traffic.

Linux subsequently copied the unmounted FAT partition, passed `fsck.fat -n`
and independently extracted both expected files. The target SHA-256 is now
`9617f2f3370ba58e837c0c7f5a1ebff0d1949c07268dd9ca6c0fd6268d2b780c`.
The three raw reference hashes still match. ROOBI recovered with boot ID
`a6a13da0-52b7-4de1-b6f0-aecb9ae716df`. Serial capture completed with 529,066
bytes and no transport errors; the NanoKVM guard disarmed.

This establishes orderly shutdown and power-button startup persistence for the
bounded file workload. It does not establish abrupt power-loss durability or
electrically measured removal of the eMMC supply. The card again reported cache
disabled on both boots. The legacy clock, unforced high-memory callers and
remaining error-recovery limits still apply. Two transfer-complete diagnostics
were recorded in total; no other failed SDHCI command or kernel panic appeared.

| Evidence | Location under `/mnt/HaikuWork` |
| --- | --- |
| Trial fixture and scripts | `artifacts/mmc-shutdown/20260913T162525Z-b0639b/` |
| Native reviewed qualification | `artifacts/interactive/20260913T163033Z-bde4d4/qualification.json` |
| File write / after-startup transcripts | Same directory: `shell-20260913T163510Z-2c10d4.txt`, `shell-20260913T164112Z-f958de.txt` |
| Linux file and FAT check | `artifacts/emmc-file-readback/20260913T164254Z-4117a5/result.json` |
| Linux reference reads | `artifacts/emmc-read-reference/20260913T164326Z-f33d0c/result.json` |
| Current result index | `state/native-mmc-shutdown.json` |

The final data remains on the FAT fixture for subsequent tests. The earlier
normal-reboot qualification and its expected hashes remain preserved separately.

## Eight-bit legacy SDR and read-only data

Source `fb1811b338f1f2cf1972d034d7391eff3abfdc01` (`hrev60097+139`) selects
the eMMC width before publishing the card. The exact RK3588 profile advertises
its admitted eight-bit wiring; hosts without a wiring description retain the
existing four-bit limit. The bus manager issues the MMC width switch, changes
the host width and rereads EXT_CSD at the operational clock. Stable read-only
fields must match the initial one-bit read, and the user-area, sector-size and
cache assumptions must still hold. A command error or failed verification stops
the bus before disk publication. Opening the MMC disk preserves the verified
width; SD continues to use its existing four-bit setup.

All 105 host checks and the ARM64 build pass. New host cases cover 1/4/8-bit
sequencing, invalid widths, command/status failures, corrupt identification and
capacity data, changed partition/cache/sector-size assumptions, and preservation
of other SDHCI host-control bits. The combined QEMU suite passes raw SD/eMMC
and FAT file I/O, fresh mounts, normal reboot, backing-file hashes and filesystem
checks along with the other regressions. QEMU uses the four-bit host default;
native testing supplies the physical eight-bit evidence.

Native session `interactive/20260913T170301Z-f34335` selected and verified
eight-bit mode on two boots separated by normal PSCI reboot. Both boots pass
read-only capacity and partition geometry, three 8 MiB raw reference hashes,
and both retained FAT file hashes. Total native checked payload is 48 MiB of
raw references plus 48 MiB of file contents. The short raw integrity reads
measured 11.1–11.7 MB/s; sustained throughput remains unqualified. The external
clock setup remains 375 kHz for identification and 24 MHz for legacy operation,
with the existing nonzero host divider and no electrical card-clock measurement.

Both boots also pass twenty-five component and five setting hashes, memory/copy
checks, inspected Tracker/Deskbar desktops and DHCP at 2.5/1 Gbit/s. This trial
used a read-only RK3588 profile and issued no file writes or flush ioctls.
The SSD was not mounted or updated and stays at `+94`. CPU caller buffers above
4 GiB were not forced; the card reported its cache disabled on both boots.
Four transfer-complete diagnostics were recorded in total, with their timing
cause still unisolated. No unexpected failed SDHCI command or panic appeared.

After recovery, Linux copied all 314,572,800 FAT bytes and matched the pretrial
partition hash `5e372be233ff1e15a894f2024d1e82f914da9f4963d5b38528f67fb38d0c3522`.
Host FAT consistency checks and both extracted file hashes pass; Linux also
confirms all three raw reference hashes. ROOBI recovered with boot ID
`a84eee7f-49db-407d-b875-aa8f22bbbfa5`. Serial capture completed without transport
errors and the NanoKVM guard disarmed. The previous four-bit write/shutdown
qualification and retained fixture remain available for the next write trial.

| Evidence | Location under `/mnt/HaikuWork` |
| --- | --- |
| Image manifest | `artifacts/mmc-width-image/20260913T165646Z-bb16e9/manifest.json` |
| Image SHA-256 | `d16fc0c801d97d78ba0263d060e2a288c6b3ecb78dcf0790aa0ca000f79b5d5b` |
| Full ARM64 build | `artifacts/build-20260913T165620Z.log` |
| 105 host checks | `artifacts/mmc-width-image/20260913T165646Z-bb16e9/host-checks.log` |
| Combined QEMU result | `artifacts/qemu-shell/20260913T165737Z-8ea745/result.json` |
| Native qualification | `artifacts/interactive/20260913T170301Z-f34335/qualification.json` |
| Native raw reads | Same directory: `shell-20260913T170754Z-933b7b.txt`, `shell-20260913T171156Z-02e0a2.txt` |
| Native file reads | Same directory: `shell-20260913T170824Z-c70408.txt`, `shell-20260913T171222Z-ffbfd2.txt` |
| Linux partition/file/FS check | `artifacts/emmc-file-readback/20260913T171448Z-5e2d0b/result.json` |
| Linux reference reads | `artifacts/emmc-read-reference/20260913T171543Z-e3edbe/result.json` |

`state/native-mmc-width.json` indexes this read-only pass. Protocol references
include the [Linux MMC width validation](https://github.com/torvalds/linux/blob/v6.12/drivers/mmc/core/mmc.c)
and [EXT_CSD field definitions](https://github.com/torvalds/linux/blob/v6.12/include/linux/mmc/mmc.h).

## Eight-bit file writes and reboot persistence

The `+140` image is built from `512e1a2bcba604fbfc3f8acc0d3cf92cad8dcb29`;
only documentation changed since the qualified `+139` driver. Its private
RK3588 profile sets `read_only false`. The retained 105 host checks cover that
unchanged code, and a fresh combined QEMU run passes SD/eMMC raw and FAT I/O,
normal reboot, host backing-file checks and the other regression gates.

Native session `interactive/20260913T173214Z-2311e4` passes six fresh overwrites
of the existing 16 MiB FAT target file. An 8 MiB overwrite starts at byte zero;
five writes of 1, 513, 4,097, 131,073 and 1,048,579 bytes start at offsets
10,485,777, 10,485,887, 10,486,301, 10,486,791 and 10,489,859. Each operation
changes the previous contents, preserves the file length, and passes an explicit
device flush followed by full source/target hashes on a fresh read-only mount.
All 9,572,871 requested bytes are confined to regular-file writes on the backed-up
300 MiB partition. No raw native eMMC write is used.

After normal `shutdown -r`, both files and all three raw reference regions match
again. Across both boots, checked payload totals 192 MiB of file contents and
48 MiB of raw references. Both boots select and verify eight-bit mode, match
twenty-five component and five setting hashes, pass the memory/copy probes,
display inspected Tracker/Deskbar desktops and obtain DHCP at 2.5/1 Gbit/s.
The SSD is not mounted or updated and remains at `+94`.

Linux independently copies all 314,572,800 FAT bytes, passes `fsck.fat -n`,
extracts both files and confirms their expected hashes. The final target SHA-256
is `6368dbcfd8e9d0ec193b28988cae0fd664eedf494da0fa6adbcba1c2dc84d348`;
the complete resulting partition hash is
`0e58eb6b4e2154aa9405e44f52ece08e1d7de7af2dd9e77d54c7eedc28e1ed50`.
Linux also confirms all three reference-region hashes. ROOBI recovers with
boot ID `17ddae9c-c71e-44d1-8a25-b1a1a2f37ad7`; serial capture has no transport
errors and the NanoKVM guard disarms.

Cache is disabled on both native boots, so this exercises the CMD13
ready/transfer-state flush path. Four transfer-complete diagnostics appear;
their timing cause remains unisolated. No unexpected SDHCI command failure or
panic appears. CPU vectors above 4 GiB are not forced, and the clock setup
remains the existing legacy profile. This trial does not extend the earlier
four-bit shutdown/startup result to eight-bit operation or establish abrupt
power-loss, cached-card flush, sustained I/O or faster-mode acceptance.

| Evidence | Location under `/mnt/HaikuWork` |
| --- | --- |
| Image manifest | `artifacts/mmc-width-write-image/20260913T172438Z-310b79/manifest.json` |
| Image SHA-256 | `9b22d6ec87752d2cd73616184328da625c2d8544da96ed0dd5914ddedd6b962f` |
| ARM64 build | `artifacts/build-20260913T172332Z.log` |
| Combined QEMU | `artifacts/qemu-shell/20260913T172623Z-326036/result.json` |
| Native qualification | `artifacts/interactive/20260913T173214Z-2311e4/qualification.json` |
| Write and reboot file transcripts | Same directory: `shell-20260913T174111Z-57182a.txt`, `shell-20260913T174439Z-0c9ae6.txt` |
| Fresh fixture and expected bytes | `artifacts/mmc-width-write-fixture/20260913T172423Z-f0e444/` |
| Linux partition/file/FS check | `artifacts/emmc-file-readback/20260913T174708Z-f05753/result.json` |
| Linux reference reads | `artifacts/emmc-read-reference/20260913T174721Z-9324d5/result.json` |

`state/native-mmc-width-write.json` indexes this pass, and
`state/emmc-write-fixture.json` records the retained final bytes for subsequent
tests.

## Opt-in high-memory CPU-buffer test mode

The RK3588 profile accepts `force_high_cpu_buffers true` in the `sdhci` driver
settings. It defaults to false. When enabled, the profile publishes a 4 GiB
lower address bound to the existing I/O scheduler; lower caller addresses use
its constrained bounce buffers. The SDHCI driver validates every used physical
vector against the same bound before submitting any card command. The private
512 KiB noncacheable SDMA payload retains its separate below-4-GiB allocation.

The driver records the minimum CPU-vector address and the SDMA address after
the first successful complete read and write on each boot. A lower vector must
reject the whole operation, including when it follows a valid vector. Host
cases cover that rejection, data integrity through the private buffer, and
copy/command failures; the existing SDMA allocation-failure checks also pass.
Native address observations and independent data checks are
still required; enabling the setting or running the low-RAM QEMU suite alone
does not qualify high-memory I/O.

## Native work remaining

Extend native eight-bit operation to orderly shutdown/startup. Extend the
bounded file result to power-loss integrity, longer mixed I/O, native caller
buffers above 4 GiB and error recovery. Speed
negotiation/tuning, native cached-card flush behavior and Haiku boot from eMMC
remain open. Preserve ROOBI and its tested recovery route during these changes.

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
