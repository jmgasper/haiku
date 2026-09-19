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
| `hdmi0-con` | VOP2 video port 2, DisplayPort TX1 (`dp@fde60000` in Linux' rk3588-extra.dtsi; `0xfdec0000` is eDP0), USBDP PHY1 (`phy@fed90000`, DP lanes 2 and 3), Radxa RA620 DP-to-HDMI bridge (`radxa,ra620`, no control bus), HPD on GPIO3_D5 (`dp1_hpdin_m0`) | The captured DT has the USBDP PHY node but no `dp` node and no bridge node |

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

### Qualified +294 hardware cursor (stage 3f, probe-only profile)

The `hrev60097+294` image (source `0a301485e0`, SHA-256
`79afea4e7cf8b0353aa44659b03339742b0e95fc6b62f6baee016525e9978c0c`) passes the
host checks (167 tests), both two-boot QEMU modes and two native boots with
normal reboot, verified shutdown and automatic ROOBI recovery. It runs the
`rock5-itx-edk2-v1.1-display-cursor` profile: app_server keeps its software
pointer, and on each boot, after the inventory and accelerant checks, the
probe saved that (bitmap-less) state, showed its quadrant bitmap with the
corner at 200,200, moved it to -32,500 and to 1888,1048, hid it there and
restored the saved state, before the mode-change and DPMS checks of the
earlier stages ran unchanged. After every step the observation read ESMART3
back exactly as programmed (region control 1 with 64-pixel rows and the
clipped geometry, or 0 when hidden), the desktop window ESMART2 untouched,
and the NanoKVM capture agreed with the quadrant classifier, judged against
the previous capture: white and black quadrants, the desktop through the
transparent quadrant, the half-white blend, only the right half of the
bitmap at the left edge (32x64 from a buffer address 128 bytes in), only the
white top-left quadrant at the bottom-right corner (32x32), the plain
desktop after the hide, and the plain desktop with the software pointer
after the restore. A visual review of the captures agrees. Each request
that changes the window waits for the port's frame start (about 800 polls
of 20 µs, one frame); a move or bitmap while hidden touches nothing. The
register dump before the first programming shows the firmware's ESMART2
with AXI read ids 0x0c/0x0d on bus 1 and ESMART3 with the same ids, the
window delays 0x17171717, the port's background delay 0x34 and mixers 4 to
6 zero; after it ESMART3 carries ids 0x0e/0x0f, mixer 6 the four blend
words and the gating word bit 31 cleared, with no bus error status bit.

| Boot | Frame buffer | Cursor buffer | Show polls at 200,200 | Left edge: start, address | Corner: start | Hide polls |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `0x10013000` | `0x09b45000` | 779 | `0x01f40000`, `+0x80` | `0x04180760` | 309 |
| 2 | `0x0fb2d000` | `0x09d73000` | 431 | `0x01f40000`, `+0x80` | `0x04180760` | 400 |

- Native evidence: `artifacts/automated-display-cursor/20260918T233158Z-a4147f`
  (`qualification.json`, `cursor*-boot{1,2}.json` with the decoded control,
  the observation, the ESMART3 words and the register dumps,
  `desktop-cursor*-boot{1,2}.json` with the classifier samples and the
  reference capture, the desktop reviews, driver syslog extracts, FDT
  captures); the mode-change and DPMS records as in the earlier stages.
- Session: `artifacts/interactive/20260918T233200Z-0ced8a`; recovery boot
  `b783bbdd-d999-4ea8-b8f1-49a3245dbbd2` after verified shutdown.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260918T231454Z-5866d4` and
  `20260918T231710Z-b13b3f`; build `artifacts/build-20260918T231437Z.log`.
- Stage: `artifacts/display-cursor/20260918T231435Z-e58a06` (its controller
  and qualifier were regenerated during the stage for the reference-frame
  classifier and the record comparison; the run directory holds the
  controller used).
- Independent Linux eMMC readbacks were not run: the ROOBI sudo password is
  not available to this session.

The road to this image is retained. On +291 app_server's first ioctl
overflowed the 16 KB kernel stack with a cursor-state local
(`artifacts/display-cursor/20260918T150410Z-db669c`, run
`…automated-display-cursor/20260918T151229Z-407591`). On +292 app_server
took the hardware cursor at start and the port went dark after its first
scanline: the cursor window had been given Linux' ESMART3 read ids, which
the firmware had already given the desktop window
(`…display-cursor/20260918T152007Z-d2961f`, run `…20260918T152629Z-4b7cb7`,
image archive `…nanokvm-image-archive/…-display-cursor-292-black-frame`).
+293 rendered the cursor correctly and stopped on a controller record and
then on the restore of a state without a bitmap
(`…display-cursor/20260918T223443Z-b5f8da`, runs `…20260918T224059Z-054576`
and `…20260918T230547Z-0884e5`); the first +294 run met Tracker windows
opened through the NanoKVM's HID devices and a classifier that expected the
plain desktop (`…20260918T231927Z-55dde9`).

## Stage 3g: app_server's pointer on the hardware cursor

The `rock5-itx-edk2-v1.1-display-cursor-desktop` profile is the cursor
profile plus the accelerant's `B_SET_CURSOR_BITMAP`, `B_MOVE_CURSOR` and
`B_SHOW_CURSOR` hooks (the driver advertises them with a second flag), so
app_server hands its pointer bitmap, position and visibility to the window
at start and stops drawing the software pointer into the frame buffer; a
bitmap larger than 64x64 is refused and app_server keeps its software
pointer. The native cycle is the cursor stage's with these additions: the
inventory requires the driver's bitmap and show lines for app_server's
pointer and reads ESMART3 back enabled with that bitmap at the desktop's
centre less the hot spot; the accelerant check requires the hooks flag and
a desktop-blue centre pixel in the frame buffer (no software pointer there);
the probe's restore brings app_server's bitmap back; the pointer window is
read back disabled at 720p (the centre is off that frame) and enabled again
at 1080p and after power-on; and a pointer classifier (light and dark
pixels over the desktop in the box at the pointer's position, plain desktop
around it) judges the captures after inventory, the restore, the return to
1080p and power-on.

### Qualified +297 app_server's pointer on the hardware cursor (stage 3g)

The `hrev60097+297` image (source `5428293910`, the +294 driver with the
`rock5-itx-edk2-v1.1-display-cursor-desktop` profile; SHA-256
`da1dda49f68b7a96440fb1bcfe3723dbd636d507e237e49b1ffc791052760a82`) passes
the host checks (168 tests), both two-boot QEMU modes and two native boots
with normal reboot, verified shutdown and automatic ROOBI recovery. On both
boots app_server handed its 22x22 pointer (hot spot 1,1) to the window at
start: the driver logged the bitmap and the show, the observation read
ESMART3 back enabled with 64-pixel rows, a 22x22 region and the start
0x021a03be (958,538, the desktop centre less the hot spot), the frame
buffer's centre pixel was the desktop blue (no software pointer drawn
there), and the NanoKVM captured Haiku's hand pointer at the centre. The
probe's placements, clipping and hide of its quadrant bitmap passed as on
+294, and its restore brought app_server's bitmap back at 959,539 with the
pointer visible again. Through the 720p change the pointer window stayed
enabled at the same start (958,538 lies inside a 1280x720 frame; the scaled
capture shows the pointer enlarged) and it read back enabled after the
return to 1080p and after DPMS on, with the pointer in every capture; the
mode-change, DPMS, retrace, observation and EDID checks passed unchanged. A
visual review of the captures agrees.

| Boot | Frame buffer | Pointer show polls | Restore show polls | Off: hold polls, time | On: time, frame starts |
| --- | --- | --- | --- | --- | --- |
| 1 | `0x104d6000` | 496 | 411 | 6, 6.0 ms | 0.62 ms, 5000 → 5030 |
| 2 | `0x0fd11000` | 668 | 507 | 4, 4.0 ms | 0.59 ms, 5098 → 5128 |

- Native evidence: `artifacts/automated-display-cursor-desktop/20260919T000434Z-d3ebe7`
  (`qualification.json` with `pointers` and `cursors`, `cursor*-boot{1,2}.json`,
  `desktop-*-boot{1,2}.json` with the pointer classifier's samples, the
  desktop reviews, driver syslog extracts, FDT captures).
- Session: `artifacts/interactive/20260919T000435Z-4ef8ec`; recovery boot
  `37964857-f1a1-4769-b0d2-84f68d347bb9` after verified shutdown.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260918T234806Z-edac25` and
  `20260918T235025Z-febe41`; build `artifacts/build-20260918T234749Z.log`.
- Stage: `artifacts/display-cursor-desktop/20260918T234552Z-84f213` (its
  first run, `…automated-display-cursor-desktop/20260918T235251Z-5974de`,
  passed every board check and stopped on the controller's expectation that
  the pointer window be disabled at 720p; the controls were corrected and
  the cycle rerun on the same image).
- Independent Linux eMMC readbacks were not run: the ROOBI sudo password is
  not available to this session.

With this image the HDMI1 "2D" set is complete: EDID, a driver-owned frame
buffer, vertical retrace, mode changes, DPMS and a hardware cursor carrying
app_server's pointer. No drawing is accelerated: app_server's blits and
fills stay on the CPU.

## Stage 4a: the DisplayPort path observed

The second connector's path is DisplayPort TX1 (`0xfde60000`, the VO0
domain, `pclk_dp1` at CLKGATE_CON(56) bit 5 with the 16 MHz AUX clock at
bit 3 and `clk_dp1` at bit 9), USBDP PHY1 (`0xfed90000`, `pclk_usbdpphy1`
at CLKGATE_CON(72) bit 4, the immortal clock at CLKGATE_CON(2) bit 15, its
GRF at `0xfd5cc000`, the lane mux and hot-plug trigger in VO0 GRF
`0xfd5a6000`), the RA620 bridge and the hot-plug pin GPIO3_D5 (function 5
`dp1_hpdin_m0` in the bus IOC's GPIO3D high mux word, level in GPIO3's
external port word bit 29). The firmware device tree carries the PHY, both
GRFs, the IOC and GPIO3 but no DP node, so the resource description
(version 2) takes the DP block from Linux' rk3588-extra.dtsi as a constant
and the rest from the tree. The observation (snapshot version 2) reads
three more CRU gate words, the two GRFs and the IOC mux words always, GPIO3
while its APB clock is ungated, and the DP block (version, type, id, the
three configuration words, CCTL, soft reset, video sample control, video
config 1, PHY interface control and power-down) while VO0 is on and
`pclk_dp1` ungated, with its AUX status, general interrupt and hot-plug
status words only while the AUX clock is ungated as well, since an
unclocked block faults the bus (the +284 lesson). Nothing is written. The
probe prints the words and a decoded path line; the validator re-derives
the gating and decodes the pin mux and level and the DP block's version
and hot-plug state. The stage runs the qualified desktop-cursor cycle with
this observation; what it records - whether the firmware leaves the block
clocked, what the pin mux is, and the hot-plug level without a sink - sets
the bring-up stage's starting point.

### Observed +300 DisplayPort path (stage 4a)

The `hrev60097+300` image (source `ba1e072979`, SHA-256
`aaeec1267d393d3c0f0d1bb612da49f7704cc2dbec7188e333946b1ee94a91e3`) passes
the host checks (170 tests), both two-boot QEMU modes and two native boots
of the desktop-cursor cycle (every check of +297 again) with normal reboot,
verified shutdown and automatic ROOBI recovery, and the observation reads
the DisplayPort path on both boots without touching it. As the firmware
leaves it, with nothing on the second connector:

| Item | Boot 1 and boot 2 |
| --- | --- |
| VO0 domain | on |
| `pclk_dp1`, AUX 16 MHz, `clk_dp1`, `pclk_usbdpphy1`, immortal, `pclk_gpio3` | all ungated |
| DP TX1 version words | `0x3231312a`, `0x65613038` (ASCII "211*", "ea08"), id `0x900116c3` |
| DP TX1 configuration | `0x0221250d`, `0x08001000`, `0x0000016c`; CCTL `0x4`; soft reset `0` |
| DP TX1 PHY interface | `0x0006f000`: power-down state 3, lanes busy, transmit off; power-down word `0` |
| DP TX1 AUX status, interrupts, hot-plug status | `0`, `0`, `0` (no hot-plug) |
| USBDP PHY1 GRF | CON1 `0x6000` (low power and LFPS bits), the rest `0` |
| VO0 GRF | `0x40`, `0`, `0xe4` (lane order 3,2,1,0) |
| GPIO3_D5 (hot-plug) | mux 0 (GPIO, not `dp1_hpdin`), input, level 0 |
| GPIO3 | data/direction `0x8100`, external port `0x0d7ca98f`, version `0x0101157c` |

The block is powered, clocked and idle, its PHY interface parked, the PHY
in low power, and the hot-plug pin muxed as a plain input reading low:
the firmware never touched this path, and without a sink the RA620 does
not raise hot-plug (whether it does with one is the first thing to learn
once a monitor or dummy plug is on that connector). Every value agreed
between the inventory's three samples, the observation after the EDID
read and both boots.

- Native evidence: `artifacts/automated-display-dp-observe/20260919T004036Z-633120`
  (`qualification.json` with `dp_path`, `display-boot{1,2}.json` with the
  decoded `dp1`, `gpio3`, `dp_pin`, GRF and IOC words, the cursor, mode,
  power and desktop records as in +297).
- Session: `artifacts/interactive/20260919T004037Z-523fe8`; recovery boot
  `251c8c12-f46c-47f9-8ef0-80594a76cc20` after verified shutdown.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260919T003537Z-b2f988` and
  `20260919T003804Z-eb45af`; build `artifacts/build-20260919T003518Z.log`.
- Stage: `artifacts/display-dp-observe/20260919T003356Z-6a9bf5`.
- Independent Linux eMMC readbacks were not run: the ROOBI sudo password is
  not available to this session.

## Stages 4b and 4c: DisplayPort TX1 trained and the first picture

With the NanoKVM moved to the second connector, the `display-dp-aux` driver
profile lets the probe drive the whole path (`--dp [edid] [train [video]]`,
`kDpProbe`, request version 2). It muxes GPIO3_D5 to `dp1_hpdin_m0`, gives
the AUX engine its clock (`clk_aux16m_1` = GPLL/75, CLKSEL_CON(117) bits
15:8), and waits for the controller's PLUG state. It then initialises
USBDP PHY1 exactly as Linux does in DP+USB mode, with DP on PHY lanes 2 and 3
(`rockchip,dp-lane-mux = <2 3>`). It reads DPCD 0x000-0x00f and the sink
count over native AUX and EDID block 0 over I2C-over-AUX. Link training
follows Linux' `dw_dp_link_train`: clock recovery and equalization with the
sink's swing and pre-emphasis requests, the drive tables on PHY lanes 2 and
3, and a rate downgrade on failure. The video step then checks that the GPLL
is 1188 MHz and runs `dclk_vop1_src` at GPLL/8 (148.5 MHz). It ungates and
selects `dclk_vop1` and programs video port 1 for CEA 1920x1080@60 with a
magenta background and no window. DP1 is muxed to the port with positive
syncs (`DSP_IF_EN` bit 1, mux 15:14 = 1; `DSP_IF_POL` 14:12 = 3), and the
DW DP stream is set up as `dw_dp_video_enable` does: quad pixel, RGB 8 bpc,
MSA, CONFIG1-5 and the horizontal blanking interval. The port must be in
standby, so a running port is never taken over. Nothing on HDMI1 or video
port 2 changes.

### Qualified +306 first picture on the second connector (stage 4c)

The `hrev60097+306` image (source `0505348cb2`, SHA-256
`88e11a6031904c603fd7daaf83775c89309d6f8e234745ea3e5a6fb88683e033`) passes
the host checks (171 tests), both QEMU modes and two native boots with
normal reboot, verified shutdown and automatic ROOBI recovery. On both
boots the NanoKVM, behind the RA620, captures a uniform 1920x1080 magenta
frame from the second connector (mean 225,41,233; all sampled pixels within
tolerance of the programmed background).

| Item | Boot 1 and boot 2 |
| --- | --- |
| Hot-plug | PLUG after 504 polls of 200 µs (~100 ms) |
| DPCD | `1414c481011001810200...`: DP 1.4, 5.4 Gb/s, 4 lanes, enhanced framing, TPS3, sink count 1 |
| EDID | "RGT" 1920x1080, valid checksum |
| AUX | 33 transfers, 9 deferred replies retried |
| Link | 5.4 Gb/s × 2 lanes on the first attempt (the board wires 2), CR 2 loops, EQ 1 loop, swing 1, pre-emphasis 0, lanes and alignment done (`770081`) |
| Clocks | GPLL `m=198 p=2 s=1` (1188 MHz); CLKSEL_CON(111) `0x201` → `0xe01` |
| Video port 1 | `0x8000000f` (standby) → `0x0000000f`; `VP_CLK_CTRL` `0xa` |
| Interfaces | `DSP_IF_EN` `0x00082021` → `0x00086023` |
| DW DP | TU 26.4 bytes, threshold 40, hblank interval 254, `VSAMPLE` `0x00410020` |
| Probe time | ~151 ms including training |

On the first board attempt with this image, the controller captured the
frame right after the probe and saw black (16,16,16), because the RA620
and the NanoKVM were still locking. The capture a few seconds later was
magenta. The controller now recaptures up to four times, five seconds
apart, and keeps every attempt; one extra capture was needed on each boot.
Earlier runs: +302 (AUX only) timed out on its first AUX request because
the AUX clock divider was never set. The +304 image carried a stale
pre-training driver object with the training-aware probe. An invalidated
build had compiled it while its header was being edited, and it ended up
newer than the header's final save. `build_info.py` now touches every
changed file when a build is invalidated. Both runs recovered normally and
their images are archived (`display-dp-aux-304-stale-driver`,
`display-dp-video-306-early-capture`).

- Native evidence: `artifacts/automated-display-dp-video/20260919T044007Z-04d01f`
  (`qualification.json`, `dp-probe-boot{1,2}.json`, the observation before
  and after the probe, and the frames with their colour checks).
- Session: `artifacts/interactive/20260919T044009Z-9fbfef`; recovery boot
  `2da2c020-5ee4-491d-b4be-58c5f931da21` after verified shutdown; image
  archive `artifacts/nanokvm-image-archive/20260919T044803Z-6193aa`.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260919T042450Z-fb1979` and
  `20260919T042716Z-b05ad0`; build `artifacts/build-20260919T042402Z.log`.
- Stage: `artifacts/display-dp-video/20260919T042226Z-297c09`.

### Qualified +308 desktop window on the second connector (stage 4d)

With `--dp edid train video window` (`kDpProbeWindow`, request version 3),
after the background the probe lets ESMART0 scan the firmware's desktop
buffer on video port 1. ESMART0 is overlay layer 2, inside video port 1's
range of the firmware's layer map (`OVL_LAYER_SEL` `0x76543210`,
`OVL_PORT_SEL` `0xa5a47738`), above the disabled Cluster0/1. It takes the
desktop window's buffer, stride, size, format, AXI bus and pipeline delay.
Its read ids sit four above the desktop's, and the cursor keeps the two in
between. It also copies the mixer words the firmware left for the desktop's
equivalent position on video port 2 (MIX4/MIX5 into MIX0/MIX1). As in Linux,
automatic clock gating is cleared before the window is enabled. With no
sink on HDMI1, EDK2 runs video port 2 at 640x480 from a 640x480 XRGB buffer
at `0xed940000`, which is the desktop app_server draws on.

The `hrev60097+308` image (source `df0b291e69`, SHA-256
`10f18fa231a23b2bee44e34af7829232bf1438cc44d28423d7487147fe662070`) passes
the host checks (172 tests), both QEMU modes and two native boots with
normal reboot, verified shutdown and automatic ROOBI recovery. On both boots
the NanoKVM captures Haiku's desktop from the second connector: Tracker's
icons, the Deskbar and the pointer fill the top-left 640x480. The magenta
background covers the rest of the 1920x1080 frame, with every sampled
pixel outside the window at the background colour and none inside it.
Video port 2 and its window are unchanged.

| Item | Boot 1 and boot 2 |
| --- | --- |
| Link | 5.4 Gb/s × 2 lanes, first attempt |
| Desktop window | ESMART2, `0xed940000`, stride 640 pixels, 640x480, XRGB8888 |
| ESMART0 | region control `0x1`, read ids `0x10`/`0x11` (`CTRL1` `0x00011100`), AXI bus 1 |
| Mixers MIX0/MIX1/MIX4/MIX5 | all zero as the firmware leaves them (no blending set up; the bottom window is opaque) |
| `SMART_DLY_NUM` | `0x17171717` (already equal) |
| `SYS_AUTO_GATING_CTRL` | `0xffffffff` → `0x7fffffff` |
| Commit | 833 polls of 20 µs (about one frame) |
| Probe time | ~168 ms |

- Native evidence: `artifacts/automated-display-dp-window/20260919T050545Z-1c8e2b`.
- Session: `artifacts/interactive/20260919T050547Z-2872a2`; recovery boot
  `ba64fc97-397d-4d25-beef-dd39515870fd`; image archive
  `artifacts/nanokvm-image-archive/20260919T051335Z-db6de6`.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260919T050056Z-062acd` and
  `20260919T050323Z-570016`; build `artifacts/build-20260919T050038Z.log`.
- Stage: `artifacts/display-dp-window/20260919T045911Z-1ebab0`.

### Qualified +310 app_server on the second connector (stage 4e)

The `rock5-itx-edk2-v1.1-display-dp-desktop` profile sends the accelerant's
frame buffer to DP1. When app_server acquires it, the driver brings the path
up as the probe does and first clones the firmware desktop, which is the
qualified +308 state. It then swaps ESMART0 to its own 1920x1080 buffer over
a black background, moves the kernel console there, and arms video port 1's
frame-start interrupt for the retrace semaphore. The shared information
carries the CEA 1080p timing (the same port words as HDMI1 at 1080p) and the
sink's EDID read over AUX. On release, ESMART0 goes back to the firmware
desktop. A later acquisition in the same boot only moves the window. HDMI1's
port and window are never touched. Mode changes, power control and the
cursor stay off on this profile.

The `hrev60097+310` image (source `9524dfb16b`, SHA-256
`92c01dea6673128acf7e985ae7b086f641723d01bafee42db54990d6fc95ffee`) passes
the host checks, both QEMU modes and two native boots with normal reboot,
verified shutdown and automatic ROOBI recovery. On both boots app_server
runs the 1920x1080 desktop on the DisplayPort-bridged HDMI port, and the
NanoKVM captures the full desktop with Tracker and the Deskbar at first try.

| Item | Boot 1 | Boot 2 |
| --- | --- | --- |
| Driver log | `dp desktop probe result=0 phase=10 link=0x14x2 edid=128 ... swap=0 polls=833` | same |
| Frame buffer | `0x104dc000`, port 1, window 0, firmware `0xed940000` | `0x0f832000`, same |
| Accelerant flags | 7 (acquired, EDID, retrace), 1 mode, "RK3588 VOP2 DP TX1" | same |
| Retrace | 24 waits, period 16666 µs, count +24 over 398 ms, 0 spurious | 24 waits, 16666 µs, +24 over 385 ms, 0 spurious |
| Observations | ports 1 and 2 active, DP1 muxed to port 1, ESMART0 on the buffer, ESMART2 on the firmware desktop | same |

- Native evidence: `artifacts/automated-display-dp-desktop/20260919T053124Z-d3a4d1`.
- Session: `artifacts/interactive/20260919T053126Z-5d7b84`; recovery boot
  `d89100de-db4a-4288-845d-6a6d3d3e5de1`; image archive
  `artifacts/nanokvm-image-archive/20260919T053848Z-3ab800`.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260919T052636Z-8edfc1` and
  `20260919T052859Z-f2338a`; build `artifacts/build-20260919T052618Z.log`.
- Stage: `artifacts/display-dp-desktop/20260919T052432Z-82a73f`.

### Qualified +313 desktop spanning both connectors (stage 5a)

The `rock5-itx-edk2-v1.1-display-dp-span` profile makes the DP desktop
acquisition allocate one 3840x1080 frame buffer with a 15360-byte row
pitch. DP1's window (ESMART0, video port 1) scans the right half. HDMI1's
window (ESMART2, video port 2) takes the left half at the same pitch, and
the qualified mode set raises HDMI1's port to CEA 1080p60 from the 640x480
the firmware runs without a sink. The mode set runs before the retrace
handler is installed, so the port stop polls its own hold-valid status.
app_server sees one 3840x1080 mode with the horizontal timing doubled
(297 MHz, 4400 total), which keeps 60 Hz. Release restores HDMI1's firmware
mode (640x480 at 25.175 MHz), window, pitch and size, then puts DP1's window
back on the firmware desktop.

The `hrev60097+313` image (source `fa0a76bbd8`, SHA-256
`2a6774c10b228285082135591e8291df06f2a0a9fd6d81ee81e77c9fad9fcdcb`) passes
the host checks, both QEMU modes and two native boots with normal reboot,
verified shutdown and automatic ROOBI recovery. On both boots app_server
runs one 3840x1080 desktop. The NanoKVM on DP1 captures its right half:
the workspace and the Deskbar at the right edge, with none of Tracker's
icons, which sit on the left half. The pointer at the desktop's centre
(x = 1920) is cut at DP1's left edge, so its other half is on HDMI1. HDMI1
has no sink, so its half is checked by read-back. Its port runs 1080p
(`0898002c,00c00840,04650005,00290461`, out of standby), and its window scans
the buffer's start at the 3840-pixel pitch with the 1920x1080 size. Both
interfaces stay muxed to their ports, and retrace runs at 60 Hz from DP1's
port.

| Item | Boot 1 | Boot 2 |
| --- | --- | --- |
| Frame buffer | `0x104dc000`, 3840x1080, pitch 15360 | `0x0f6b2000`, same |
| HDMI1 mode set | result 0, phase 6, port 2, window 2 (firmware `0xed940000`, 640x480) | same |
| Windows | ESMART2 at the buffer, ESMART0 at buffer + 7680, both pitch 3840 | same |
| Accelerant | flags 7, 1 mode, "RK3588 VOP2 HDMI TX1 + DP TX1" | same |
| Retrace | 24 waits, 16666 µs, count +24, 0 spurious | same |

- Native evidence: `artifacts/automated-display-dp-span/20260919T055844Z-2aaf07`.
- Session: `artifacts/interactive/20260919T055845Z-c06816`; recovery boot
  `02385ea7-f6c5-4fc8-b06c-d310b0b63957`; image archive
  `artifacts/nanokvm-image-archive/20260919T060632Z-3f1060`.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260919T055350Z-82784a` and
  `20260919T055617Z-41f9dc`; build `artifacts/build-20260919T055333Z.log`.
- Stage: `artifacts/display-dp-span/20260919T055154Z-302f39`.
- Open: a picture check of HDMI1's half needs a monitor or dummy plug on
  HDMI1 (only one NanoKVM).

### Qualified +315 spanning desktop with a hardware cursor per port (stage 5b)

With the `rock5-itx-edk2-v1.1-display-dp-span-cursor` profile, the spanning
desktop gets app_server's pointer on one cursor window per port. HDMI1's
qualified ESMART3 (mixer 6) covers the left screen. ESMART1, overlay layer 3
of video port 1 with mixer 2, covers the right one, with the pointer shifted
by 1920. Each window is clipped to its own screen, so a pointer on the seam
shows its halves on both ports. Hiding and release cover both windows, and
the DP profiles never admit HDMI1's mode set or power control.

By then a monitor was attached to HDMI1, and EDK2 started HDMI1 at 1080p
(firmware buffer `0xed280000`) instead of the sink-less 640x480
(`0xed940000`). The span code needed no change, and the controllers now
accept either firmware state.

The `hrev60097+315` image (source `9bf36a3e87`, SHA-256
`1be8daa51fa390e55975404a718612da210b2800382e7591929de637cc04a127`) passes
the host checks, both QEMU modes and two native boots with normal reboot,
verified shutdown and automatic ROOBI recovery. The first board attempt
recovered normally but failed the old 640x480-only syslog check; its image
is archived as `display-dp-span-cursor-315-hdmi1-sink-check`. On both
qualified boots:

- the accelerant reports flags 55 (acquired, EDID, retrace, cursor, cursor
  hooks), and app_server's 22x22 pointer goes to the cursor windows;
- HDMI1's port is at 1080p, its window on the buffer's left half at the
  3840 pitch, and DP1's on the right half;
- the probe's quadrant cursor, placed at desktop (2900, 500), shows on DP1
  at (980, 500) over the right half of the desktop in the NanoKVM capture;
- the owner watched the HDMI1 monitor during the run and reported "Yes, I
  see the left half of the desktop" (`state/hdmi1-owner-observation.json`,
  also in the qualification). This is the picture evidence for HDMI1's half,
  which no capture device sees.

- Native evidence: `artifacts/automated-display-dp-span-cursor/20260919T063337Z-e07d1f`.
- Session: `artifacts/interactive/20260919T063339Z-c765eb`; recovery boot
  `c0c62cf6-5138-4553-bd50-0c2a81f41235`; image archive
  `artifacts/nanokvm-image-archive/20260919T064114Z-f1e689`.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260919T061812Z-47de6a` and
  `20260919T062039Z-bb4994`; build `artifacts/build-20260919T061754Z.log`.
- Stage: `artifacts/display-dp-span-cursor/20260919T061616Z-39c30a`.

## Later stages

3. Modes beyond the PLL table (the fractional-rate calculation) or the
   frame buffer size, and accelerated blits and fills (app_server's
   `B_FILL_RECTANGLE`/`B_SCREEN_TO_SCREEN_BLIT` hooks) on the RGA or the
   GPU, which nothing here provides yet.
4. A window on video port 1 scanning its own frame buffer on the trained
   DP1 link, then the second connector as a second screen for app_server
   (mode setting from the sink's EDID, link retraining on hot-plug).
5. One wide framebuffer scanned by two video ports for a spanning desktop.
