# The DSI display connector on the Raspberry Pi 4

State 2026-10-08: the Waveshare 3.5" DSI LCD (E) (640x480, capacitive
touch) on the board's DISPLAY connector shows AirTop's CPU graph, network
graph and temperature. The panel is an *auxiliary display*: programs draw
on it through the Device Kit's `BAuxDisplay`; it is not part of
app_server's desktop. Touch is not read yet (see Open).

## Pieces

- `rpi_dsi` (`src/add-ons/kernel/drivers/graphics/rpi_dsi`): the display
  pipeline for DSI1. Device `/dev/auxdisplay/rpi_dsi/0`, protocol
  `headers/os/drivers/auxdisplay.h` (generic: any auxiliary display driver
  speaks it) plus the lab ioctls of `headers/private/graphics/rpi_dsi/rpi_dsi.h`.
- `BAuxDisplay` (`headers/os/device/AuxDisplay.h`, libdevice): finds the
  displays under `/dev/auxdisplay`, hands out a `BBitmap` of the display's
  size that accepts views, `Present()` copies it into the next hardware
  buffer. Also power, backlight and touch calls for drivers that have them.
- `rpi_thermal` (`src/add-ons/kernel/drivers/power/rpi_thermal`): the SoC
  temperature from the firmware as `/dev/power/rpi_thermal/0`, in the text
  form of the other thermal drivers, so AirTop's temperature panel works on
  the Pi.
- AirTop (`/mnt/HaikuWork/apps/AirTop`): `AuxPanel` draws the panel at 15
  frames a second from the sampler's data; `AirTop --panel` runs it without
  a window (the Pi image's UserBootscript starts that), launching AirTop
  again opens the window beside it; the View menu switches the panel.
- `rpi4_dsi` (`tools/rpi4/dsi.cpp`, lab image): state, the enable sequence
  step by step, registers, display list memory, colour bars, a frame dump
  (`rpi4_dsi dump f.raw`, rows of B_RGB32 after a header line) to look at
  the panel's picture from the build host.
- Panel timings: built in for the Waveshare 3.5" (E); any plain video-mode
  panel through `settings/kernel/drivers/rpi_dsi` (sample in
  `data/settings/kernel/drivers`).

## How the pipeline works

The firmware drives both HDMI outputs and owns the compositor (HVS) for
them. It does not know this panel (its DSI support is the official 7"
display only: the boot log says `pin DISPLAY_DSI_PORT not defined`). So the
driver takes the parts the firmware leaves idle, programmed the way Linux's
vc4 driver programs them (reference sources in
`/mnt/HaikuWork/rpi4/reference/vc4`):

1. **Clocks** (`brcm,bcm2711-cprman`): PLLD runs at 3 GHz; its DSI1 channel
   gets an integer divider for the panel's bit clock (24 bits per pixel per
   lane: 640x480 at 24 MHz wants 576 MHz, divider 5 gives 600 MHz, so the
   pixel clock becomes 25 MHz and the front porch grows from 48 to 81 pixels
   to keep 60 Hz, as Linux does). The escape clock `DSI1E` is 100 MHz from
   PLLD_PER (750 MHz, fractional divider 7.5).
2. **Power domain**: the DSI1 block's analog front end is in the firmware's
   power domain 19 (`SET_DOMAIN_STATE`). The device tree binding calls it 18;
   Linux's raspberrypi-power driver adds one. With it off, every register
   write seems to work but the PHY never transmits (`STAT` shows an LP1
   contention error and no HS clock) and the pixel valve gets no clock.
3. **DSI1** (`brcm,bcm2711-dsi1`, 0xfe700000): soft reset, the AFE's bias
   and lane power-down bits, the pixel clock `DSI1P` from the PHY's byte
   clock, HS/LP timing registers from the unit interval, `PHYC` with the
   lanes and continuous HS clock, DISP0 in video mode with `PIX_CLK_DIV` =
   24 / lanes and RGB888; enabled last.
4. **HVS**: channel 2 is free (the firmware uses 0 and 1 for the HDMI
   outputs and gave channel 2 a 3840-pixel output buffer). A nine-word
   display list (one unscaled RGBA8888 plane, pixel order ARGB, then the end
   word) sits at word 4080 of the 4096-word list memory, far above the
   firmware's lists (820 and 1636; the driver checks they do not reach it).
   Output 3 (`DSP3_MUX` in `DISPCTRL`) is routed to the channel.
5. **Pixel valve 1** (0xfe207000): the timings, DSI format 24, FIFO level
   64 - 3 x 6, clock from DSI; `PV_CONTROL_EN` then `VIDEN`.

Presenting another buffer rewrites the pointer word of the display list;
the HVS reads it at the next frame. Switching off runs the steps backwards
and powers the domain down; the HDMI outputs are not touched at any point.

## Things learned

- `STAT` bit 17 (`PHY_CLOCK_HS`) and `DISPSTAT2` mode 2 (RUN) with a moving
  frame counter are the signs of a running pipeline; `PV_STAT` 0x844.
- The HVS display list memory beyond the firmware's lists holds random data
  (never written), a good sign it is unused.
- The AFE register `PHY_AFEC0` does not read back its bias/IDR fields.
- A panel frame costs app_server about 7 ms of software drawing; at five
  frames a second AirTop's panel is about one percent of the system. (A
  first version looked ten times dearer: BApplication hands the launch's
  own arguments to ArgvReceived, which had opened the dashboard window.)
- The firmware reports the DSI1 power domain off at boot; nothing else
  uses it.
- The lab's HDMI0 cable was not connected to the NanoKVM on 2026-10-08
  (no signal), so the pictures were checked with `rpi4_dsi dump` (the
  panel's scan-out buffer) and the MCP server's screenshots (the desktop).

## Open

- Touch: the panel's Goodix GT911 sits on the connector's I2C (GPIO 44/45,
  BSC0 at 0xfe205000, address 0x14 or 0x5d). Needs an I2C bus driver for
  the BCM2835 BSC and a small touch driver feeding
  `AUX_DISPLAY_WAIT_TOUCH`; the protocol is ready for it.
- No backlight control (the panel has none).
- One client at a time is assumed; nothing arbitrates several programs.
- A rotated panel (portrait) would be a plane transform or drawing-side.
