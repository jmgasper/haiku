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

## Later stages

3. Own framebuffer and VOP2 window programming at the firmware timing, then a
   real mode change with HDPTX PHY1 and HDMI TX1 reconfiguration, vertical
   retrace from the VOP2 interrupt, cursor window and power control, exposed
   through a board accelerant.
4. DisplayPort TX1 through USBDP PHY1 and the RA620 bridge for the second
   connector. This needs a sink on that port (a monitor, an HDMI dummy plug,
   or the NanoKVM cable moved) before it can be qualified.
5. One wide framebuffer scanned by two video ports for a spanning desktop.
