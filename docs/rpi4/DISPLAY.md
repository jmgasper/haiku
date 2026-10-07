# Displays on the Raspberry Pi 4

State 2026-10-06: both HDMI outputs are displays of one desktop, arranged
with the Screen preferences (or `screenmode`). The current lab configuration
detects both displays at 1920x1080 and uses a 3840x1080 desktop. HDMI0 is
visible through the KVM; HDMI1's physical picture has not been observed.
The earlier 640x480 forced-output tests below are historical.

## How it works

The firmware keeps what it set up at boot: each output's video mode and the
compositor (HVS). air/OS does not program the HDMI hardware. Instead:

- `rpi_display` (`src/add-ons/kernel/drivers/graphics/rpi_display`) owns one
  frame buffer (contiguous, below 1 GB, write-combining) and asks the firmware
  for one plane per output with the "firmware KMS" property tag `SET_PLANE`
  (0x00048015): a source rectangle of the buffer, scaled to a destination
  rectangle on the output. No overlay in `config.txt` is needed for that.
- `rpi_display.accelerant` implements the fork's display layout hooks
  (`B_GET_DISPLAY_OUTPUTS`, `B_SET_DISPLAY_LAYOUT`, ...). app_server arranges
  the outputs; a layout becomes one frame buffer the size of the regions'
  bounding box, and the driver points each output's plane at its region.

Because the firmware scales, an output's *resolution* and its *scale* are both
just the size of its region: 1280x720 on a 1080p monitor, or 1920x1080 at
150 %, is a 1280x720 region shown full screen. A mirror shows its source's
region, letterboxed if the shapes differ. The kernel console follows the
frame buffer (`frame_buffer_update`); when app_server lets go, the planes are
removed and the firmware's own frame buffer (the boot console) shows again.

`config.txt` has `max_framebuffers=2` so that the firmware sets up both
outputs.

## Things to know about the firmware interface

- A plane names its display by the firmware's display **id** (2 = HDMI0,
  7 = HDMI1; `FRAMEBUFFER_GET_DISPLAY_ID`), not by the display number. With
  the number, the request is accepted and nothing shows.
- Plane ids count across the displays (three per display, as Linux's
  firmware-KMS driver has them). Two displays using plane id 0 ends with the
  second plane on the first display.
- The buffer address is a VideoCore bus address (physical | 0xc0000000).
- `GET_EDID_BLOCK_DISPLAY` answers for a display without a monitor with the
  other display's EDID; the driver drops such a copy.
- The number of displays is what the firmware saw at boot. There is no hot
  plug: a monitor connected later needs a restart.

## Earlier checks on the board (2026-10-03)

| What | How | Result |
|---|---|---|
| Two outputs in app_server | `screenmode -d` | HDMI-1 1920x1080 (EDID name, size), HDMI-2 640x480; desktop 2560x1080 |
| Lower resolution | `screenmode --display 1 --display-mode 1280x720` | region 1280x720 shown 1920x1080 |
| Scale | `--display 1 --scale 150` | region 1280x720, UI enlarged (`evidence/layout-swap.jpg`) |
| Arrangement | `--position` on both | HDMI0 shows the right-hand part of the desktop |
| Mirror | `--display 1 --mirror 2` | HDMI0 shows the 640x480 desktop letterboxed (`evidence/layout-mirror.jpg`) |
| Layout kept over a restart | restart | the saved layout comes back |
| Screen preferences | `evidence/screen-prefs.jpg` | both displays, arrangement, mirror, per-display resolution and scale |
| OpenGL | GLTeapot | renders as before (`evidence/teapot-rpidisplay.jpg`) |

## The lab's HDMI1

During the 3 October tests, the firmware reported one display unless
`hdmi_force_hotplug:1=1` was added to
`config.txt`, and read no EDID from HDMI1. The Linux recovery system saw
the same ("Registered
framebuffer for display 0" only). The cause was not established. By the
6 October performance run both outputs supplied 1080p modes without that
forced-output setting. This establishes detection, not HDMI1 picture quality;
the Dell monitor still needs a physical check.

## Open

- No mode setting: the outputs stay at the firmware's modes (the monitors'
  preferred modes). `SET_TIMING` (0x00048017) would be the way.
- No hot plug, no DPMS, no hardware cursor (app_server draws the pointer),
  no vertical retrace semaphore.
- An output cannot be given more pixels than its mode (no shrinking).
