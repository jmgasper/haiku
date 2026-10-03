# air/OS on the Raspberry Pi 4: status

Board in the lab: Raspberry Pi 4 Model B rev 1.5, 4 GB (board code c03115),
EEPROM bootloader 2022-01-25, firmware 1.20260915. Branch `rpi4`.

| # | Stage | State | Evidence |
| --- | --- | --- | --- |
| 1 | SD card / boot | **Works on the board.** The minimum image boots from the SD card to the desktop, 1920x1080 on HDMI0, four cores at 1.5 GHz, serial console through loader and kernel | 2026-10-03: KVM screenshot of the desktop; serial capture `evidence/serial/boot3.log` |
| 2 | Ethernet | **Works on the board.** `bcm_genet`: 1000 Mbit/s full duplex, DHCP address, the lab's telnet shell and file push run over it | 2026-10-03: `evidence/serial/boot6.log`, `shell.py 192.168.1.209 ifconfig` |
| 3 | USB | **Works on the board.** `<pci>bcm2711` brings the PCIe link up and has the VL805's firmware loaded; the xHCI driver runs it with uncached DMA memory. USB 3 flash drive, the NanoKVM's disk, keyboard, mouse and tablet enumerate; a click sent through the KVM opens the Deskbar menu | 2026-10-03: `evidence/serial/boot9.log`, `listusb`, `evidence/usb-click2.jpg` |
| 4 | GPU (Vulkan / OpenGL) | **OpenGL works on the GPU; Vulkan open.** Kernel: the `v3d` driver (power, buffers through the MMU, bin/render, TFU and compute jobs in submission order). User space: Mesa 25.3.6's `v3d` Gallium driver built for Haiku (`tools/rpi4/mesa`). GLTeapot renders in a window at about 315 FPS; `rpi4_gl_probe` (offscreen GLES, renderer "V3D 4.2.14.0", OpenGL ES 3.1) is pixel-exact up to 1920x1080 with and without a depth buffer. Mesa, the vendor file, GLTeapot and GL Info are in the `rpi4-airos` profile; a card flashed from scratch runs GLTeapot at about 300 FPS (`evidence/stage5-clean-teapot.jpg`). **Open:** Vulkan (v3dv), presentation without a copy. See `GPU.md` | 2026-10-03: `evidence/teapot6.jpg`, `evidence/glinfo.jpg`, probe output over telnet |
| 5 | Multi-display + Screen preferences | **Works through the firmware's compositor; HDMI1's picture not seen.** `rpi_display` driver + accelerant: one frame buffer, one firmware plane per HDMI output, the fork's display layout hooks. Screen preferences and `screenmode` arrange the two outputs, mirror them, and set per-display resolution and scale (the firmware scales); the layout survives a restart. The lab's HDMI1 monitor is not detected by the firmware, so output 2 ran forced at 640x480 and unseen. **Open:** real mode setting, hot plug, DPMS, hardware cursor. See `DISPLAY.md` | 2026-10-03: `evidence/layout-*.jpg`, `evidence/screen-prefs.jpg`, `screenmode -d` over telnet |
| 6 | Wi-Fi + Wi-Fi tool | **Works: scan, WPA2 join, traffic.** `broadcomfmac`: OpenBSD's `bwfm` on the compatibility layer with an SDIO host of its own for the CYW43455. Joined "Gaspers" (WPA2-PSK/CCMP, 802.11ac), DHCP, ping 1.4 ms, 8 MB transfer with matching checksum, reachable over Wi-Fi alone with Ethernet down; scan while connected; leave and rejoin. The Wi-Fi preferences lists the networks (`evidence/wifi-prefs.jpg`). Driver and firmware are in the `rpi4-airos` profile; a card flashed from scratch on 2026-10-04 scans with both Wi-Fi and Bluetooth. **Open:** join by clicking in the Wi-Fi tool not tried (KVM mouse input broken on this image: xHCI "TRB" transfer errors on the NanoKVM's HID endpoints), polling instead of the card interrupt, throughput unmeasured. See `WIFI.md` | 2026-10-03: `ifconfig` output and pings over telnet, `evidence/wifi-prefs.jpg` |
| 7 | Bluetooth / BLE + Bluetooth tool | **The controller is up; LE scan, connect and GATT work.** `h4bcm`: an H4 transport on the mini UART for the BCM4345C0, with the patch file loaded at open and the board's address from the device tree. The Bluetooth preferences shows the controller and nearby devices; `bt_le` scans, connects to an LE device and reads its services. Driver and patch file are in the image profile. **Open:** pairing and profiles untested (no device to pair here), 115200 baud. See `BLUETOOTH.md` | 2026-10-04: `evidence/bluetooth-prefs.jpg`, `bt_le` output over telnet |
| 8 | Media decoding, airTime | **airTime runs; software decoding only, no sound.** The arm64 airTime package plays H.264 1080p30 at 30 fps with libavcodec; HEVC 1080p30 reaches 18.8 fps. **Open:** sound output (HDMI and jack need VCHIQ), hardware decoding (firmware codec through VCHIQ for H.264, the HEVC block). See `MEDIA.md` | 2026-10-04: `evidence/airtime1.jpg`, airTime Stats over telnet |
| 9 | Summit with GPU and WebGL | Not started | - |
| 10 | The other applications | **All but Aurora are installed and start.** From `/mnt/HaikuWork/apps`: airShot, Amp (tasamp), Burrow, Kiri, LCDMonitor, Natter, TurboChook, airTime and Summit come as the arm64 packages of the ROCK 5 image; Clipper got an arm64 package (`tools/airos/build-arm64-app-packages.sh ... clipper`). Each was started on the board and stayed running; windows on screen in `evidence/apps1.jpg`. Nothing beyond starting was tested per app. **Open:** Aurora (Electron apps) needs Node.js, which has no arm64 Haiku build | 2026-10-04: launch check over telnet, `evidence/apps1.jpg` |

Also done, outside the stage list:

- **The full air/OS image** (`jam -q @rpi4-airos build airos-rpi-image`, 4 GB):
  branding, the owner's applications, Summit, firmware packages. **Runs on
  the board** (2026-10-03, `evidence/airos-board.jpg`,
  `evidence/serial/airos2.log`): branded desktop at 1920x1080, Ethernet up,
  the applications and packages in place. The applications themselves have
  not been started there yet.
- **Restart** through the BCM2835 watchdog works on the board (`shutdown -r`
  over telnet, `evidence/serial/boot8.log` shows the EEPROM banner again).
  Shutdown (stay off) is not exercised yet.
- **Regression check of the shared changes** (fdt `ranges` translation, PL011
  set-up in the kernel, framebuffer driver remap order): an EFI image from
  the same tree boots in QEMU's `virt` machine with a readable serial console
  (2026-10-03).

Open points from stages 2 and 3:

- GENET runs promiscuous with one queue and copies every packet; no
  checksum offload, no multicast filter.
- The PCIe inbound window covers the first 4 GB at PCI address 0: fine for
  the 1/2/4 GB boards, not for RAM above 4 GB on the 8 GB board.
- xHCI interrupts are INTx (no MSI controller driver).
- USB HID logs a stalled control request and a "Context state" error when
  the NanoKVM's keyboard is set up; input works regardless.
- throughput is unmeasured for both.

## What stage 1 consists of

- `haiku_loader.rpi`, a boot platform started by the firmware (BOOT.md).
- `airos-boot.tgz`, the boot archive: kernel and the modules needed to mount
  the boot volume.
- `bcm2711_emmc2`, the SD controller driver.
- `fdt`: `get_reg()` translates addresses through the parent buses' `ranges`.
- arm64 kernel: sets the PL011's baud rate itself when the loader passes the
  UART clock; `boot_splash` copes with a loader that showed no splash.
- jam: `airos-rpi-boot-archive`, `airos-rpi-image`; profile `minimum-rpi4`.

## Known gaps in stage 1

- **No restart or shutdown.** The kernel only knows PSCI, which the Pi's
  firmware does not provide. Needs a watchdog (`bcm2835-pm`) driver.
- **The boot volume is found by looking at every BFS partition.** With a
  second air/OS disk attached (USB, later) the choice is not defined; the
  plan is an ID in `cmdline.txt`.
- **Loader menu needs the serial console**; the loader has no USB driver.
- **SD speed**: 25 MHz default speed only (no high speed, no UHS).
- **SD hot-plug**: the card is only looked for when the bus comes up.
- `disable-bt` gives the PL011 to the serial console; Bluetooth needs it
  back (or the mini UART as console) later.
- Only the 4 GB rev 1.5 board has been run. 8 GB boards and the B0 chip
  revision (DMA limited to the first gigabyte) are handled in the code but
  untested.

## Emulation

`tools/rpi4/qemu-run.sh` runs the same loader, archive and card image in
QEMU's `raspi4b`. Differences that matter: QEMU puts the SD card on the
legacy SDHCI (the lab's QEMU device tree moves the emmc2 node there) and
that model has no DMA (the driver falls back to the data register); QEMU
emulates neither PCIe nor GENET.

## Open: USB input through the NanoKVM is unreliable

Seen 2026-10-03 on the full image: on most boots the NanoKVM's keyboard,
mouse and tablet interfaces stop early ("transfer error on slot 3 endpoint 9
or 11: TRB", then `usb_hid: error waiting for report`), and later the hub's
own control requests time out ("hub 7: error updating port status"). Of the
six or so boots checked, a KVM click worked on one (the Deskbar opened) and
input stopped again within minutes. The same errors are in serial logs back to stage 3, so
this is not new with the display or Wi-Fi drivers. Flash drive and RNDIS on
the same hub keep working. Not yet understood: the errors hit interrupt IN
endpoints of the high speed composite device; suspects are the interrupt
endpoint setup in xhci for the VL805 and lost PCIe interrupts (the rings are
in uncached memory and a full barrier precedes each doorbell, so stale ring
contents are unlikely). One real
defect was fixed on the way (low/full speed interrupt endpoints were
configured with a zero payload per interval).

Tried on 2026-10-04 and taken out again, because the error stayed (always
"TRB" on the tablet's interrupt IN endpoint, 6 byte packets, at the first
transfer after usb_hid starts):
- Raspberry Pi's Linux tree marks the VL805 with `XHCI_AVOID_DQ_ON_LINK` ("the
  xHC does not correctly parse link TRBs if the HW dequeue pointer is set to
  one"). This driver links every transfer into the ring with a Link TRB, and
  restarts a ring at its first entry. Starting rings with a No Op entry did
  not remove the error, and control transfers failed twice on that boot.
- Limiting interrupt IN transfers to one packet (in case the controller
  refuses transfers longer than the endpoint's Max ESIT Payload): no change.
The other VL805 quirks of that tree (`XHCI_EP_CTX_BROKEN_DCS`,
`XHCI_ZHAOXIN_TRB_FETCH`, `XHCI_VLI_HUB_TT_QUIRK`) have not been looked at. Whether the Logitech mouse on
the other port works was not checked: nobody can see or move it from here.
