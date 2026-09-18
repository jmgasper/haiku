# Native display control

The desktop currently uses the EFI framebuffer that EDK2 v1.1 programs before
Haiku starts. This page records the board's display wiring, the firmware
configuration Haiku inherits, and the staged work toward native VOP2/HDMI
control, a second output and a spanning desktop. Every stage keeps the
qualification rules of the [GPU work](GPU.md): host fixtures, the full ARM64
build, both QEMU modes, two native boots, recovery and independent integrity
checks before any claim. Nothing here accelerates drawing; app_server no
longer calls accelerant fill or blit hooks, so "2D acceleration" on this board
means native mode setting, hardware cursor, vertical retrace and power control
through a board accelerant, with GPU-rendered content presented by Mesa.

## Board wiring

The mainline `rk3588-rock-5-itx.dts` (Linux 6.18.52) describes the two HDMI
connectors asymmetrically:

| Connector | Path | Haiku device-tree presence |
| --- | --- | --- |
| `hdmi1-con` | VOP2 video port 1, DW HDMI QP TX1 (`hdmi@fdea0000`), Samsung HDPTX PHY1 (`phy@fed70000`) | Present and enabled in the EDK2 mainline DT that Haiku captures |
| `hdmi0-con` | VOP2 video port 2, DisplayPort TX1 (`dp@fdec0000`), USBDP PHY1 (`phy@fed90000`, DP lanes 2 and 3), Radxa RA620 DP-to-HDMI bridge (`radxa,ra620`, no control bus) | The captured DT has the USBDP PHY node but no `dp` node and no bridge node |

HDMI TX0 (`hdmi@fde80000`) is not wired to a connector and is disabled.

EDK2 v1.1 (`edk2-rk3588` 6a682c0e, `edk2-rockchip` fbe0805b) configures
`PcdDisplayConnectors = {HDMI1, DP0}` and `PcdDp1LaneMux = {0}` for this board,
so the firmware framebuffer and the NanoKVM capture use the native HDMI1 path
and the RA620 port is never driven by firmware. The fetched firmware display
sources (VOP2, DW HDMI QP, HDPTX PHY, DP and EDID code) are verified by git
object id in `artifacts/firmware-source/edk2-rk3588-v1.1/display-sources-receipt.json`.

## Stage 1: read-only observation (+259)

The `rk3588_display` kernel driver admits exactly the ROCK 5 ITX description:
the VOP2 node, its video port 1 endpoint leading to the enabled HDMI TX1
node, that controller's HDPTX PHY and GRF, the SYS/VOP/VO1 GRF syscons, the
PMU and the CRU. Register bases, interrupt routes (VOP SPI 156; HDMI TX1 SPIs
173-176 and 361), clock identities and power domains (VOP 24, VO1 26) are
compared against the recorded firmware description before anything is mapped.
Phandles stay dynamic. It publishes `/dev/graphics/rk3588_display/0`, which
opens read-only and answers two ioctls: the resource description and a
snapshot.

A snapshot reads the always-on PMU, CRU and GRF words first, then maps VOP2
(8 KiB) only while the VOP power domain is on and its bus clocks are ungated,
and HDMI TX1 (16 KiB) only while the VO1 domain is on and its APB clock is
ungated. It records VOP2 version, interface enables and muxes, overlay
selection, all four video-port timings, cluster and ESMART window controls
and addresses, HDMI TX1 video-interface status and the HDMI hot-plug levels.
Nothing is written. The host fixture runs the production admission, gating,
cleanup and ioctl paths against a modeled device tree and guarded register
pages, including 75 rejected description faults and mapping failures at every
step. The validator re-derives timing and routing from the raw words and
requires three consecutive samples to agree.

### Retained +259 failure

The first native +259 boot passed the Mali firmware, resource and platform
regressions, published `/dev/graphics/rk3588_display/0`, rejected a writable
open and reported the matching resource description, then panicked inside the
snapshot with a synchronous external abort (`ESR 96000010`,
`FAR ffff0000022160ec`) while reading HDMI TX1 offset `0xec`, the write-only
`I2CM_CONTROL0` software-reset register. VOP2 reads had completed. Neither
Linux nor the firmware ever reads that register. The guarded session's
emergency recovery returned ROOBI (boot `897a16e0-6c7c-4652-9569-55355b88ca49`)
through the corrected firmware boot order. The panic, its controller record and
the used image are retained:
`artifacts/automated-display-observe/20260918T033856Z-0336d2`,
`artifacts/interactive/20260918T033858Z-59a66f` and
`artifacts/nanokvm-image-archive/20260918T034713Z-afdc15-display-observe-panic`.

The corrected driver reads only HDMI TX1 registers that the reference drivers
read or read-modify-write (`GLOBAL_SWDISABLE`, the I2C master interface
controls, `AUDIO_INTERFACE_CONFIG0`, `HDCP2LOGIC_CONFIG0`, `LINK_CONFIG0`, the
packet scheduler configuration/enable words and the main-unit interrupt
status/mask), and the host fixture rejects any other offset.

### Qualified +263 observation

The corrected `hrev60097+263` image (source `eb2cef4e29`, SHA-256
`ad16ced6d81893da89e764d8e1982e35fbea4ee7e75981d42ba74cc5fdc85eb9`) passes
154 host checks, both two-boot QEMU modes (the driver correctly publishes no
device without a VOP2 node) and two native boots with normal reboot, verified
shutdown and automatic ROOBI recovery. Each boot reads three identical
snapshots without a register write; the Mali firmware, resource and platform
regressions pass alongside. Both boots inherit the same firmware display state:

| Observation | Value |
| --- | --- |
| VOP2 version | `0x40176786` |
| Interface enables (`DSP_IF_EN`) | HDMI1 only, fed by video port 2 (`hdmi_edp1_mux` = 2) |
| Video port 2 timing | 2200 x 1125 total, 1920 x 1080 active (h 192-2112, v 41-1121), hsync end 44, vsync end 5, out mode 15; ports 0, 1 and 3 in standby |
| Scanout window | ESMART2 region 0 enabled, buffer `0xed280000`, virtual width 1920, 1920 x 1080 at origin |
| Hot-plug (`SOC_STATUS1`) | HDMI1 level and interrupt bits set; HDMI0 clear |
| HDPTX PHY1 GRF status | PLL lock, clock ready and PHY ready set |
| HDMI TX1 | video path enabled, TMDS link (`LINK_CONFIG0` = 0), AVI and GCP packets scheduled, no pending I2C status |
| Power and clocks | VOP, VO0 and VO1 domains on; VOP and HDMI TX1 bus clocks ungated; dclk selectors 111-113 = `0x201`, `0x1001`, `0x5` |

So the firmware drives the NanoKVM's HDMI1 port from **video port 2**, not the
video port 1 that the mainline device tree assigns to HDMI1. Stage 3 must
follow the firmware routing. The desktops on both boots were actually viewed
(normal Tracker/Deskbar desktop, no error dialog). The `SOC_STATUS1` low bits
change between samples; the validator compares only its hot-plug bits. The
earlier +263 run was rejected for exactly that and its used image is archived
(`artifacts/nanokvm-image-archive/20260918T041540Z-c89dc4-display-observe-validator-rejected`);
a second run failed only on a controller assertion because the minimum image
has no `grep` (`...20260918T043352Z-4d1501-display-observe-syslog-assert`).

- Native evidence: `artifacts/automated-display-observe/20260918T043836Z-49720b`
  (`qualification.json`, `display-boot{1,2}.json`, desktop reviews, FDT captures).
- Session: `artifacts/interactive/20260918T043838Z-ce5f59`; recovery boot
  `5956166f-fbc5-4e1c-9878-9e3b8cd45ab6` after verified shutdown.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260918T035509Z-9056c6` and
  `20260918T035529Z-4dcff7`; build `artifacts/build-20260918T035351Z.log`.
- Stage: `artifacts/display-observe/20260918T035350Z-e1424f`; used image
  archive `artifacts/nanokvm-image-archive/20260918T044908Z-dea7fd`.
- Independent Linux eMMC readbacks were not run for this stage: the ROOBI sudo
  password is not available to this session.

## Stage 2: EDID over the HDMI TX1 I2C master

The opt-in `rock5-itx-edk2-v1.1-display-edid` profile in the `rk3588_display`
driver settings enables an EDID ioctl. It re-checks the VO1 domain, the HDMI
APB clock and the HDMI1 hot-plug level, maps the HDMI TX1 window writable and
reads one 128-byte block per request through the controller's I2C master,
one byte per transfer as Linux and the firmware do, polling the done/error
status with a bounded deadline. A NACK or timeout resets the master and
clears the request bits. Segment reads use the DDC segment pointer for blocks
2 and 3. Video, PHY, clock and power registers are untouched. The host
fixture models the I2C master synchronously and checks data, segment use,
NACK and timeout handling, mapping failures and gating. The validator decodes
the base block, its preferred timing and the extension blocks independently.

### Qualified +264 EDID read

The `hrev60097+264` image (source `28cfe534a9`, SHA-256
`c95f9f922f8500ccecfdf150ab87011ebd6498c5700153419383624a03a47888`) passes the
host checks, both two-boot QEMU modes (no device without a VOP2 node; the EDID
mode fails cleanly) and two native boots with normal reboot, verified shutdown
and automatic ROOBI recovery. On each boot the inventory reads three identical
snapshots, then both EDID blocks, then three more snapshots that match the
first set in power, gating, hot-plug, interface and timing state. The Mali
firmware, resource and platform regressions pass alongside and the desktop was
viewed on both boots.

| EDID field (NanoKVM HDMI sink) | Value |
| --- | --- |
| Manufacturer, product, serial | `VCS`, `0x1145`, `0x4515b1`; 2021, EDID 1.3, digital |
| Extensions | one CEA-861 block (tag `0x02`, revision 3); both checksums valid |
| Preferred timing | 1920x1080, 148.5 MHz pixel clock, 280/45 blanking, 60.0 Hz |
| Transfer | 128 one-byte reads per block, 2560 status polls, about 51.7 ms per block, identical on both boots |
| Master state after each block | request bits clear, no pending done/NACK status, hot-plug level set |

- Native evidence: `artifacts/automated-display-edid/20260918T051843Z-7f2d0c`
  (`qualification.json`, `edid-boot{1,2}.json`, `display-boot{1,2}.json`,
  desktop reviews, FDT captures, driver syslog extracts).
- Session: `artifacts/interactive/20260918T051845Z-398455`; recovery boot
  `e68f4a2f-9808-4853-8321-e92304fe9290` after verified shutdown.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260918T043332Z-196567` and
  `20260918T044627Z-6e0b38` (an earlier EL1 attempt, `20260918T043352Z-8dd42d`,
  lost its USB shell marker and was rerun); build `artifacts/build-20260918T042045Z.log`.
- Stage: `artifacts/display-edid/20260918T034024Z-b7ba19`. A first native run
  (`artifacts/automated-display-edid/20260918T050549Z-388b64`) passed the same
  checks but its controller mislabelled the inventory events through a
  shadowed loop variable; it is retained and the corrected controller was
  rerun on the same image. Used image archive:
  `artifacts/nanokvm-image-archive/20260918T052837Z-2ce714`.
- Independent Linux eMMC readbacks were not run: the ROOBI sudo password is
  not available to this session.

## Stage 3a: scanout buffer swap

The opt-in `rock5-itx-edk2-v1.1-display-scanout` profile adds the first VOP2
write path. A swap request re-checks the VOP power domain and bus clocks, maps
VOP2 writable, and locates the single enabled ESMART window that feeds the
live HDMI1 video port (the port comes from the `DSP_IF_EN` mux, so it follows
the firmware's VP2 routing). The window must be XRGB8888, 1920 words wide,
1920x1080 at the origin, and its buffer address must equal the framebuffer
boot item's physical address. The driver then fills a physically contiguous
8 MiB colour-bar pattern below 4 GiB (eight vertical bars inside a grey
border), evicts it from the cache and retypes it write-combining, writes the
window's `REGION0_YRGB_MST` and commits with `REG_CFG_DONE` for that port
only. The window registers are shadowed: the port keeps its `REG_CFG_DONE`
bit set until its next frame start loads the new set, and reads return the
active values. The driver therefore polls that bit (20 µs steps, at most
100 ms) before verifying the address read-back, as mainline Linux does before
sending a flip's vblank event. Restore writes the firmware address back the
same way; closing the device restores a pending swap. Timing, PHY, clock and
power registers are untouched, and queries map VOP2 read-only.

The host fixture models VOP2 with the words observed on hardware, logs every
changed word of a writable mapping in order and admits only those two
offsets; it covers gating, every geometry deviation, boot-item mismatches,
allocation failures, verify failures and restore-on-close. The native cycle
captures a NanoKVM frame about 11 s into a 30 s hold and classifies its bar
colours, then checks that the desktop frame after the restore no longer shows
the pattern and that the observation is unchanged.

### Retained +267 verify failure

The first native run of the swap (`hrev60097+267`, stage
`artifacts/display-scanout/20260918T053010Z-fe133a`, both QEMU gates passed)
located the window correctly (port 2, ESMART2, `0xed280000`), allocated the
pattern and issued the two writes, but verified the address immediately after
the commit and read the still-active firmware address, so it reported
`VerifyFailed` and restored on close; the probe aborted before the hold and
the run was recovered without a clean shutdown. No panic, no other register
was touched, and the observation and EDID checks of that boot passed.
Evidence: `artifacts/automated-display-scanout/20260918T053658Z-78e3c4`,
session `artifacts/interactive/20260918T053659Z-1ac5e5` (recovery boot
`fef4908d-9235-4afc-b023-d1d064bdc38e`), used image archive
`artifacts/nanokvm-image-archive/20260918T054342Z-d485f9-display-scanout-verify-failed`.
The fixture now models the shadowed registers and the commit bit, including a
port that never takes the commit (bounded timeout, swap kept pending for the
restore on close).

### Qualified +269 scanout swap

The corrected `hrev60097+269` image (source `23b4eebf6c`, SHA-256
`79de4d2691ef350d9560452ca430857e450be40224de778b916d7daa6922d441`) passes
the host checks (159 tests), both two-boot QEMU modes and two native boots
with normal reboot, verified shutdown and automatic ROOBI recovery. On each
boot, after the observation and EDID inventory, the probe swapped the live
window to the driver pattern, held it for 30 s, and restored it; the NanoKVM
frame captured during the hold shows the eight colour bars and grey border
(classifier: 28 of 28 samples correct; also viewed), the frame after the
restore shows the normal desktop, and the observation after the swap equals
the one before it. This is the first VOP2 register write from Haiku on the
board.

| Boot | Pattern buffer | Show (polls, time) | Restore (polls, time) |
| --- | --- | --- | --- |
| 1 | `0x14c55000` | 240 polls, 12.4 ms | 731 polls, 14.6 ms |
| 2 | `0x148d1000` | 790 polls, 23.4 ms | 738 polls, 14.8 ms |

Both swaps used video port 2 and ESMART2 with the firmware address
`0xed280000`, wrote only `REGION0_YRGB_MST` and `REG_CFG_DONE`
(`0x00048004`), and completed within two frame periods. The driver never had
to restore on close. The two boots' pattern buffers differ only because the
contiguous allocation landed elsewhere.

- Native evidence: `artifacts/automated-display-scanout/20260918T055256Z-9be223`
  (`qualification.json`, `scanout-boot{1,2}.json`, `pattern-boot{1,2}.json`,
  `after-scanout-boot{1,2}.json`, desktop and pattern reviews, observation and
  EDID decodes, FDT captures, driver syslog extracts).
- Session: `artifacts/interactive/20260918T055257Z-442e25` (pattern frames
  `frame-009.jpg` and `frame-024.jpg`); recovery boot
  `e8d0f2f4-b701-44fd-baa7-3806e5571b60` after verified shutdown.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260918T054755Z-02bf6b` and
  `20260918T055012Z-b681aa`; build `artifacts/build-20260918T054708Z.log`.
- Stage: `artifacts/display-scanout/20260918T054742Z-b09571`; used image
  archive `artifacts/nanokvm-image-archive/20260918T060137Z-080d21`.
- Independent Linux eMMC readbacks were not run: the ROOBI sudo password is
  not available to this session.

## Stage 3b: board accelerant on a driver-owned frame buffer

The opt-in `rock5-itx-edk2-v1.1-display-accelerant` profile turns the device
into app_server's graphics device. It then admits writable handles and
answers `B_GET_ACCELERANT_SIGNATURE` with `rk3588_display.accelerant`, so
app_server's scan of `/dev/graphics` picks it instead of the EFI framebuffer
device (the Mali device still rejects writable opens). The primary
accelerant asks the driver to acquire the frame buffer: the driver checks
that the live HDMI1 window still scans the firmware buffer the boot item
describes, reads the sink's EDID base block, decodes the port's timing words
into sync positions, allocates a black contiguous write-combining buffer of
the firmware size, swaps the window to it exactly as in stage 3a (waiting for
the configuration-done bit) and moves the kernel console to the new buffer.
It publishes a cloneable shared area with the mode, timing and EDID, and
clones the buffer into app_server on request. Closing the acquiring handle
swaps the firmware buffer back, returns the console and detaches every
clone before the areas go away. A second acquisition is refused, the pattern
swap is refused while the buffer is acquired, and every other profile keeps
rejecting writable opens, so nothing changes for the earlier stages.

The accelerant itself exports the hooks app_server requires and nothing
more: one 1920x1080 32-bit mode at the firmware timing (148.5 MHz, sync
2008/2052/2200 and 1084/1089/1125), the EDID decoded from the shared block,
the frame buffer configuration and pixel-clock limits. There is no
acceleration, cursor, retrace semaphore or DPMS yet. The host fixture models
the shared area, clones, console updates and each refusal and failure
cleanup; the probe's `--accelerant` mode reads the driver's description
through a second writable handle, maps the live buffer once and confirms
that a second acquisition is refused; the validator cross-checks the
observation (the window must scan the accelerant's buffer), the EDID
inventory and the captured desktop frame.

## Stage 3c: vertical retrace from the frame-start interrupt

Once the frame buffer is acquired the driver keeps VOP2 mapped, installs a
handler on the VOP interrupt (GIC 188, SPI 156, shared with nothing Haiku
drives), clears and enables the live port's frame-start field interrupt in
its `VP_INT_CLR`/`VP_INT_EN` words, and creates the retrace semaphore the
accelerant returns for `B_ACCELERANT_RETRACE_SEMAPHORE`. The handler
acknowledges whatever status the port reports, counts frame starts and
releases the semaphore only towards threads already waiting on it, so the
count never runs away while app_server is idle. Release disables the
interrupt, removes the handler and deletes the semaphore before the buffer
swap back. If the interrupt or semaphore cannot be set up the frame buffer
still works without retrace. These three interrupt words are the only VOP2
registers added to the two of the swap. The probe waits for 24 frame starts
and reports their spacing and the driver's count; the validator requires a
60 Hz period and a count that grew with the elapsed frames.

### Qualified +277 accelerant with retrace (stages 3b and 3c)

The `hrev60097+277` image (source `8bba87274f`, SHA-256
`4918f10b0130d8c1a4715000ec2ca668b844b00a9c3df0de6f1105674eceadd9`) passes the
host checks (162 tests), both two-boot QEMU modes (an EL2 attempt lost its
USB shell marker and was rerun) and two native boots with normal reboot,
verified shutdown and automatic ROOBI recovery. On each boot app_server came
up on `rk3588_display.accelerant`: the driver acquired the frame buffer at
boot, the desktop the NanoKVM captures is drawn into that buffer (the
classifier and a visual review agree), the observation shows ESMART2 scanning
it instead of the firmware address, the probe's second writable handle read
the same description, mapped the live buffer (desktop blue at the corners,
the cursor pixel at the centre) and was refused a second acquisition, and
the observation and EDID inventory were unchanged by all of it. Retrace: the
probe waited for 24 consecutive frame starts on both boots without a timeout
or error, the spacing was 16 666 µs (60.0 Hz) and the driver's count grew by
exactly 24; the handler had seen no spurious interrupt since boot.

| Boot | Frame buffer | Acquire polls | Frame starts before the probe | Retrace period |
| --- | --- | --- | --- | --- |
| 1 | `0x103c9000` | 815 | 1249 (from 67.6 s after boot) | 16 666 µs |
| 2 | `0x0fad8000` | 275 | 1274 (from 71.5 s after boot) | 16 666 µs |

- Native evidence: `artifacts/automated-display-retrace/20260918T083448Z-7a36c4`
  (`qualification.json`, `accelerant-boot{1,2}.json`, desktop classifier
  records for every captured frame, desktop reviews, observation and EDID
  decodes, FDT captures, driver syslog extracts).
- Session: `artifacts/interactive/20260918T083449Z-28d1a5`; recovery boot
  `49e4982b-35c3-4b79-96c3-5cb046102bee` after verified shutdown.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260918T081918Z-719a17` and
  `20260918T080902Z-a8009c` (flaked EL2 attempt `20260918T080420Z-2c820d`);
  build `artifacts/build-20260918T080405Z.log`.
- Stage: `artifacts/display-retrace/20260918T080419Z-7af0a1`; used image
  archive `artifacts/nanokvm-image-archive/20260918T084317Z-1a648f`.
- Independent Linux eMMC readbacks were not run: the ROOBI sudo password is
  not available to this session.

The road to this image is retained. The +272 accelerant candidate
(`artifacts/display-accelerant/20260918T064244Z-886333`) ran three native
cycles: its first boot passed every check with the desktop on `0x10533000`,
but the controller mis-counted the persisted syslog on boot 2, the NanoKVM
storage was full for the second attempt, and the third used a validator that
already required the retrace fields (`artifacts/automated-display-accelerant/
20260918T064853Z-863365`, `…T065712Z-59f0db`, `…T072043Z-7aa4b5`). The +274
retrace candidate booted with every opt-in path disabled because its settings
named a profile the driver did not know (`…display-retrace/20260918T072645Z-7a4f01`).
On +275 and +276 the frame-start interrupts ran at 60 Hz but the probe's
waits failed at once: the kernel logged "tried to acquire kernel semaphore",
because a semaphore created in the kernel is not acquirable from userland
(`…20260918T073737Z-5a710a`, `…20260918T075542Z-25cfba`). Handing it to the
acquiring team, as intel_extreme does, fixed that; the first +277 run then
tripped only on a controller assertion that the reboot-time "released" line
reaches the syslog (`…20260918T082618Z-d018b8`).

## Stage 3d: native mode changes on HDMI1

The opt-in `rock5-itx-edk2-v1.1-display-modeset` profile admits a mode
change of the acquired frame buffer's port through the accelerant's
`B_SET_DISPLAY_MODE` (the accelerant then lists the sink's EDID modes whose
pixel clocks the PHY PLL table can produce, at or below 1920x1080) or through
the probe. The sequence follows mainline Linux 6.18 with the values EDK2
v1.1 uses on this board (`state/hdmi1-modeset-notes.md` records both): the
port is put into standby and the driver waits for its `DSP_HOLD_VALID`
report through the retrace handler; the PHY is powered down with its APB
reset pulsed and the init, common and lane resets asserted; the ROPLL is
programmed for the TMDS character rate from the same table Linux and EDK2
use, the PLL is enabled and the GRF's clock-ready bit awaited; the port's
timing, line flag, post-processing and pre-scan words and the window's
visible size are written and committed, and the port leaves standby; the
lanes are configured and the PHY-ready and PLL-lock bits awaited; finally
the AVI infoframe carries the new CEA VIC and AVMUTE is cleared. The pixel
clock is the PHY PLL's pixel output, which the firmware had already selected
as the port's clock, so the CRU is touched only for the three PHY resets.
The frame buffer stays 1920x1080 with its stride; a smaller mode scans its
top-left part, which is what app_server draws into once the accelerant
reports the new mode. The host fixture models the PHY, the HDMI TX packet
words, the HIWORD GRF and CRU words and the port's hold report, checks the
whole write sequence for 1280x720@60 and each timeout, and the native cycle
switches to 1280x720@60 and back on each boot, checking the observation,
the accelerant and the NanoKVM capture at each size.

### Qualified +282 mode changes (stage 3d)

The `hrev60097+282` image (source `e55761012a`, SHA-256
`3a3b80617655346a492443675f2f8955587e93a60a8a2d354dcf4b8fafec250d`) passes the
host checks (165 tests), both two-boot QEMU modes and two native boots with
normal reboot, verified shutdown and automatic ROOBI recovery. On each boot,
after the inventory and accelerant checks of the earlier stages passed
unchanged (app_server on the accelerant, the desktop in the driver's buffer,
24 retrace waits at 16 666 µs), the probe asked the driver for 1280x720@60
(CEA VIC 4) and then for 1920x1080@60 (VIC 16). Every change completed at
phase 6: the port reported standby within a few polls, the PHY clock was
ready after 5 polls and the lanes locked after 1, the port timing read back
as programmed, and the retrace measurement afterwards still gave 24
consecutive frame starts at 16 666 µs. The observation after each change
showed one live port at the new size with the ESMART2 window's visible size
following it and the power domains unchanged, and the accelerant reported
the new mode with its CEA timing at the buffer's 7680-byte pitch. The
NanoKVM captured the 720p signal as the desktop's top-left 1280x720 crop
enlarged to its 1920x1080 output (larger icons, no Deskbar, the cursor at
the scaled position) and the 1080p return as the normal desktop; the
classifier and a visual review agree. One capture after each 720p change
timed out on the NanoKVM and was retried once.

| Boot | Frame buffer | 720p: hold polls, time | 1080p: hold polls, time |
| --- | --- | --- | --- |
| 1 | `0x1018a000` | 6, 6.6 ms | 3, 3.6 ms |
| 2 | `0x0f881000` | 12, 12.6 ms | 9, 9.6 ms |

- Native evidence: `artifacts/automated-display-modeset/20260918T103134Z-1553a1`
  (`qualification.json`, `mode_720-boot{1,2}.json` and `mode_1080-boot{1,2}.json`
  with the change, the observation and the accelerant state after it, crop
  and desktop classifier records for every captured frame, desktop reviews,
  driver syslog extracts, FDT captures).
- Session: `artifacts/interactive/20260918T103136Z-8eebb6`; recovery boot
  `e2fc0b76-1ce6-4b8c-8679-df917fa92862` after verified shutdown.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260918T101312Z-ff9581` and
  `20260918T101529Z-5ae25a`; build `artifacts/build-20260918T101021Z.log`.
- Stage: `artifacts/display-modeset/20260918T101310Z-1083a9`; used image
  archive `artifacts/nanokvm-image-archive/20260918T104119Z-12530a`.
- Independent Linux eMMC readbacks were not run: the ROOBI sudo password is
  not available to this session.

The road to this image is retained. The +279 candidate reached 720p on the
board (`artifacts/display-modeset/20260918T092008Z-a64434`,
`artifacts/automated-display-modeset/20260918T094056Z-aa5b69`) but its
controller insisted on the firmware geometry after the change; +280
(`…display-modeset/20260918T095627Z-9d4a1d`, `…automated-display-modeset/20260918T100101Z-326554`)
did the same and exposed the sync-polarity bits the driver had merged into
the shared flags. On +282 one start failed because an image archive held
the lab's hardware lock (`…20260918T101747Z-66243a`) and one run lost the
NanoKVM capture after the 720p change before the retry existed
(`…20260918T102507Z-3e2d44`); the archived images are
`artifacts/nanokvm-image-archive/20260918T101528Z-8cd730-display-modeset-279-failed`
and `…20260918T102004Z-9a4bd6-display-modeset-280-failed`. This image also
carries the first power-control code: app_server's start-up DPMS-on request
replayed the 1080p mode set on each boot (`power on result=0 phase=6`, 10.6
and 15.6 ms), which 213e02c5a9 turns into a no-op; power control itself is
stage 3e below and not yet qualified.

## Stage 3e: DPMS power control on HDMI1

The same profile admits power control of the acquired frame buffer's port,
through the accelerant's `B_SET_DPMS_MODE` (stand-by, suspend and off all
mean the same thing to an HDMI sink) or through the probe. Off is the first
two steps of a mode change and nothing more: the port goes to standby and
the driver waits for its `DSP_HOLD_VALID` report, then the PHY is powered
down with its resets asserted, so the sink loses its TMDS clock and the
frame-start interrupts stop. On is the full mode set of the current mode
(the firmware's, if the driver never changed it), so the port, PLL, lanes
and infoframe come back exactly as a mode change leaves them, and a mode
change while off simply starts from the stopped port. A request for the
state the port is already in touches nothing (app_server asks for DPMS on
at every start, which the driver logs as phase 0). The shared
information carries the power mode and the mode's sync polarity in their
own fields, and `B_DPMS_MODE` reports the last accepted request. While the
PHY is down the HDMI TX1 registers are unreachable (a read raised an SError
on the board), so the observation reads that block only while the GRF shows
the PHY PLL enabled and an EDID request is answered "powered off". Releasing
the frame buffer while off, or on a mode other than the firmware's, first
restores the firmware mode so the console on the firmware frame buffer is
usable again. The host fixture checks the off, off-again and on sequences
at 1080p and 720p, a mode set from the powered-off state and the
release-time restore; the native cycle powers off after the mode changes,
records what the NanoKVM captures without a signal, and powers on again,
checking the observation (port standby, no active port), the retrace count
(unchanged over half a second while off, growing again after on) and the
desktop capture after on.

### Qualified +285 DPMS power control (stage 3e)

The `hrev60097+285` image (source `e1649e849a`, SHA-256
`884d8ff441ef06216af75d03626e112b7b8cfffebebde2dd9b4d060b97f9a43d`) passes the
host checks (165 tests), both two-boot QEMU modes and two native boots with
normal reboot, verified shutdown and automatic ROOBI recovery. On each boot,
after the inventory, accelerant and 720p/1080p mode-change checks of the
earlier stages passed unchanged, the probe asked the driver for DPMS off and
then on. Off completed at phase 2: the port reported standby within a few
polls, the PHY was powered down (GRF status 0, PLL-enable clear), the
driver's frame-start count did not move over the following half second, the
observation showed no live port and skipped the unreachable HDMI TX block,
and the NanoKVM, with no input signal, had no frame to capture (its
screenshot request timed out, which the controller records as the no-signal
outcome). On completed at phase 6 in about 0.6 ms with the PHY clock ready
after 3 polls and the lanes locked after 1, the frame-start count grew by 30
in the next half second, the retrace measurement gave 24 waits at
16 666 µs, the observation showed the 1080p port live again with the HDMI
block read, and the NanoKVM captured the restored desktop (the classifier
and a visual review agree). app_server's start-up DPMS-on request was
answered as a no-op once per boot (`power on result=0 phase=0 already`).

| Boot | Frame buffer | Off: hold polls, time | On: PLL polls, time | Frame starts off, on |
| --- | --- | --- | --- | --- |
| 1 | `0x10143000` | 17, 17.0 ms | 3, 0.57 ms | 3475 → 3475, 3476 → 3506 |
| 2 | `0x0f926000` | 5, 5.0 ms | 3, 0.58 ms | 3533 → 3533, 3534 → 3564 |

- Native evidence: `artifacts/automated-display-power/20260918T122414Z-f91277`
  (`qualification.json`, `power_off-boot{1,2}.json` and `power_on-boot{1,2}.json`
  with the change, the observation and, after on, the accelerant state,
  `desktop-power_off-boot{1,2}.json` recording the absent capture, desktop
  classifier records and reviews, driver syslog extracts, FDT captures).
- Session: `artifacts/interactive/20260918T122416Z-7f1c47`; recovery boot
  `37be467f-6b95-4eb0-8c64-07d3193bb658` after verified shutdown.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260918T111706Z-2b9ef3` and
  `20260918T111922Z-28aca9`; build `artifacts/build-20260918T111650Z.log`.
- Stage: `artifacts/display-power/20260918T111705Z-2bf370` (its controller
  was regenerated twice during the stage, for the validator's PLL rule and
  for the absent capture; the run directory holds the version used); used
  image archive `artifacts/nanokvm-image-archive/20260918T123516Z-58a730`.
- Independent Linux eMMC readbacks were not run: the ROOBI sudo password is
  not available to this session.

The road to this image is retained. The +284 candidate powered the port
off correctly and then took the kernel down when the observation probe read
the HDMI TX registers with the PHY off (SError; `artifacts/display-power/20260918T104837Z-04025c`,
`artifacts/automated-display-power/20260918T105311Z-b4639d`, image archive
`artifacts/nanokvm-image-archive/20260918T111113Z-c5ef61-display-power-284-panic`).
On +285 the first run stopped on the host validator, which still expected
the HDMI block to be read (`…automated-display-power/20260918T112137Z-92c242`),
and the second on the NanoKVM capture, which never returns while there is
no signal (`…20260918T113120Z-d30ea7`).

## Stage 3f: hardware cursor on HDMI1

The `rock5-itx-edk2-v1.1-display-cursor` profile (which implies the mode-set
profile and everything below it) admits a hardware cursor window to the
probe while app_server keeps its software pointer; the
`rock5-itx-edk2-v1.1-display-cursor-desktop` profile also exports the
accelerant's cursor hooks, so app_server's pointer goes to the window. The
firmware left ESMART3 bound to video port 2 on its topmost layer
(`OVL_PORT_SEL` 0xa5a47738, `OVL_LAYER_SEL` 0x76543210: window 3 is layer
id 7 on layer 7, which alpha mixer 6 blends, as Linux' rk3588 window table
and `vop2_setup_alpha` also have it), so the driver programs that window
with a 64x64 straight-alpha ARGB buffer it allocates with the frame buffer,
using the mixer words Linux' `vop2_parse_alpha` derives for a
per-pixel-alpha source over an opaque destination (`0x00ff0125`,
`0x00ff0060`, `0x00000024`, `0x00000074`), 64-pixel rows, no scaling, no
colour key and no mirroring. The window's AXI bus and read ids follow the
desktop window's (the same bus, ids two above, which is Linux' 0x0c/0x0d
when the firmware used Linux' 0x0a/0x0b), its `SMART_DLY_NUM` pipeline
delay copies the desktop window's, and the VOP's automatic clock gating is
cleared while the window is in use, as Linux does before enabling windows,
and handed back at release. Before the first programming and after every
enable or disable the driver logs both windows' control words, the delays,
the port's background delay, mixers 4 to 6, the overlay words, the gating
word and both bus error status words.
Four ioctls carry the pointer: the bitmap (B_RGBA32 rows of at most 64x64
with the hot spot inside them), the position (clipped to the current mode's
frame at every edge; the window's address skips the cropped rows and
columns, and a pointer entirely off the frame disables the window),
visibility, and the whole state for a probe to save and restore. Every
request writes the window and mixer words, commits the port and polls the
shadowed `REG_CFG_DONE` bit before reading the region control, start and
address back. A hidden pointer only remembers its position and bitmap, a
failed hide that left the window enabled is reprogrammed by the next
request, a mode change re-clips the pointer, DPMS keeps its state, and the
release disables the window before the firmware frame buffer returns. The
accelerant exports `B_SET_CURSOR_BITMAP`, `B_MOVE_CURSOR` and
`B_SHOW_CURSOR` when the driver advertises the cursor, so app_server hands
over its pointer bitmap and position and stops drawing the software pointer
into the frame buffer (a bitmap larger than 64x64 is refused and app_server
keeps its software cursor).

The host fixture models the cursor buffer, the shadowed ESMART3 and mixer
words (pending until the modelled frame start), the delay and gating words
and checks gating and refusals, both profiles' flags, the programming
sequence, clipping at the left, top, right and bottom edges, an off-frame
pointer, new bitmaps while shown and hidden, the commit timeout, a region
control that never takes, the mode change, DPMS and the release. The probe's `--cursor X Y` saves app_server's state to a
file and shows its own 64x64 quadrant bitmap (white, black, transparent red
and half-transparent white) with its corner at X,Y, `--cursor-hide` hides
it where it is, and `--cursor-restore` puts app_server's pointer back. The
native cycle places it whole at 200,200, clipped at the left edge at
-32,500 and at the bottom-right corner at 1888,1048, hides it there and
restores, reading the window registers back through the observation after
each step and judging each NanoKVM capture (the quadrant colours, the
desktop through the transparent quadrant, the blend, the plain desktop
where the pointer was hidden), before the mode-change and DPMS checks of
the earlier stages run on the same boot.

## Later stages

3. After mode changes and power control: a cursor window, and modes beyond
   the PLL table (the fractional-rate calculation) or the frame buffer size.
4. DisplayPort TX1 through USBDP PHY1 and the RA620 bridge for the second
   connector. This needs a sink on that port (a monitor, an HDMI dummy plug,
   or the NanoKVM cable moved) before it can be qualified.
5. One wide framebuffer scanned by two video ports for a spanning desktop.
