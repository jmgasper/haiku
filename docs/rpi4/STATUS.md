# air/OS on the Raspberry Pi 4: status

Board in the lab: Raspberry Pi 4 Model B rev 1.5, 4 GB (board code c03115),
EEPROM bootloader 2022-01-25, firmware 1.20260915. Branch `rpi4`.

**The image.** `jam -q @rpi4-airos build airos-rpi-image` (after
`tools/rpi4/stage-packages.sh` and the fetch scripts) writes
`airos-rpi4.img`, 4 GB: a FAT boot partition with the Raspberry Pi firmware,
the air/OS loader and boot archive, and the BFS system partition with the
full branded air/OS. Write it to an SD card as it is. A card flashed from the
image of 2026-10-04 was checked on the board: desktop on HDMI0, Ethernet,
Wi-Fi joins a WPA2 network, Bluetooth scans, GLTeapot and Summit's WebGL
Aquarium run on the GPU side by side (`evidence/final-image.jpg`), the clock
is set from the network at start. The image of 2026-10-04 (evening), with
sound, both hardware decoders, the Wi-Fi fixes and the scheduler change, was
flashed and checked for: both decoders byte for byte against FFmpeg (25 HEVC
streams, the H.264 one), airTime's playing rates and seeks (`MEDIA.md`),
sound through the Media Kit at 48000 frames a second, Wi-Fi joining with a
1 ms ping and an idle bus thread, all the applications and Summit's WebGL
page starting (`evidence/apps-after-scheduler.jpg`). Bluetooth, the second
display's layout and GLTeapot were not gone through again on it. The image
file was then built once more with two more lab tools in it and flashed
again (the card's readback matches the file); on that card the decoders and
airTime's rates were checked again, Wi-Fi, sound and the applications were
not. The image with the Wi-Fi bus thread on its interrupt and the join at
start (sha256 a936cb51…) was flashed on 2026-10-04 (late, readback matching)
and checked for Wi-Fi only: after Gaspers was saved and the board restarted
it joined by itself; the bus thread used 0.8 % of a core joined, 0.2 % with
nothing joined, 0.04 % with the interface down; 32 MB in 3.8-3.9 s.
The image with the CPU name, the ARM clock in the device tree and the ICU
data directory (sha256 8d152a39…) was flashed on 2026-10-04 (readback
matching) and checked for About this system only, plus Wi-Fi rejoining
Gaspers once its settings file was put back (see the last section).

**What is open**, most important first: nobody has listened to the sound
output (the driver runs and HDMI0 carries audio, see `MEDIA.md`); the
firmware's H.264 decoder slows the ARM's memory down in bursts, which still
costs airTime a picture every few seconds, and 4K HEVC decodes but cannot be
shown at full rate (a video plane of the firmware's compositor would solve
both); the second HDMI output has never shown a picture to anyone in the
lab; nothing presents through Vulkan (no loader, no window system layer);
pairing a Bluetooth device and joining Wi-Fi by clicking in the tools are
untested; Aurora needs Node.js. Details in the sections below and
in the documents named in the table.

| # | Stage | State | Evidence |
| --- | --- | --- | --- |
| 1 | SD card / boot | **Works on the board.** The minimum image boots from the SD card to the desktop, 1920x1080 on HDMI0, four cores at 1.5 GHz, serial console through loader and kernel | 2026-10-03: KVM screenshot of the desktop; serial capture `evidence/serial/boot3.log` |
| 2 | Ethernet | **Works on the board.** `bcm_genet`: 1000 Mbit/s full duplex, DHCP address, the lab's telnet shell and file push run over it | 2026-10-03: `evidence/serial/boot6.log`, `shell.py 192.168.1.209 ifconfig` |
| 3 | USB | **Works on the board.** `<pci>bcm2711` brings the PCIe link up and has the VL805's firmware loaded; the xHCI driver runs it with uncached DMA memory. USB 3 flash drive, the NanoKVM's disk, keyboard, mouse and tablet enumerate; a click sent through the KVM opens the Deskbar menu | 2026-10-03: `evidence/serial/boot9.log`, `listusb`, `evidence/usb-click2.jpg` |
| 4 | GPU (Vulkan / OpenGL) | **OpenGL works on the GPU; Vulkan renders headless.** Kernel: the `v3d` driver (power, buffers through the MMU, bin/render, TFU and compute jobs in submission order, sync objects). User space: Mesa 25.3.6 built for Haiku (`tools/rpi4/mesa`): the `v3d` Gallium driver and the `v3dv` Vulkan driver. GLTeapot renders in a window at about 300 FPS, also on a card flashed from scratch; `rpi4_gl_probe` (GLES 3.1) is pixel-exact up to 1920x1080 with and without depth; `rpi4_vk_probe` ("V3D 4.2.14.0", Vulkan 1.3) reads back a clear and a triangle exactly. **Open:** a Vulkan loader and window system layer (nothing presents through Vulkan), presentation without a copy. See `GPU.md` | 2026-10-04: probe output over telnet, `evidence/teapot6.jpg`, `evidence/stage5-clean-teapot.jpg` |
| 5 | Multi-display + Screen preferences | **Works through the firmware's compositor; HDMI1's picture not seen.** `rpi_display` driver + accelerant: one frame buffer, one firmware plane per HDMI output, the fork's display layout hooks. Screen preferences and `screenmode` arrange the two outputs, mirror them, and set per-display resolution and scale (the firmware scales); the layout survives a restart. The lab's HDMI1 monitor is not detected by the firmware, so output 2 ran forced at 640x480 and unseen. **Open:** real mode setting, hot plug, DPMS, hardware cursor. See `DISPLAY.md` | 2026-10-03: `evidence/layout-*.jpg`, `evidence/screen-prefs.jpg`, `screenmode -d` over telnet |
| 6 | Wi-Fi + Wi-Fi tool | **Works: scan, WPA2 join, traffic.** `broadcomfmac`: OpenBSD's `bwfm` on the compatibility layer with an SDIO host of its own for the CYW43455. Joined "Gaspers" (WPA2-PSK/CCMP, 802.11ac), DHCP, ping 1 to 2 ms, 32 MB in 3.9 s over Wi-Fi alone with Ethernet down (65 Mbit/s); scan while connected; leave and rejoin. The bus thread, which used 80 % of a core all the time (its interrupt fired for ever), runs on the card interrupt: 0.9 % of a core joined and idle, 0.2 % with nothing joined (it was 2.6 % and 1.5 % while it also polled every millisecond after traffic); the chip's power saving, which the storm had hidden, is off. A remembered network is joined at start (`wifiautojoin` from the boot script). The Wi-Fi preferences lists the networks (`evidence/wifi-prefs.jpg`). Driver and firmware are in the `rpi4-airos` profile; a card flashed from scratch on 2026-10-04 scans with both Wi-Fi and Bluetooth. **Open:** join by clicking in the Wi-Fi tool not tried (KVM input was broken then; it works since the xHCI fix below), sending speed unmeasured. See `WIFI.md` | 2026-10-03: `ifconfig` output and pings over telnet, `evidence/wifi-prefs.jpg` |
| 7 | Bluetooth / BLE + Bluetooth tool | **The controller is up; LE scan, connect and GATT work.** `h4bcm`: an H4 transport on the mini UART for the BCM4345C0, with the patch file loaded at open and the board's address from the device tree. The Bluetooth preferences shows the controller and nearby devices; `bt_le` scans, connects to an LE device and reads its services. Driver and patch file are in the image profile. **Open:** pairing and profiles untested (no device to pair here), 115200 baud. See `BLUETOOTH.md` | 2026-10-04: `evidence/bluetooth-prefs.jpg`, `bt_le` output over telnet |
| 8 | Media decoding, airTime | **Sound output, hardware H.264 and hardware HEVC decoding work; nobody has heard the sound.** `vchiq` (the message channel to the VideoCore firmware's services), `bcm2835_audio` (multi audio driver on the firmware's sound service: HDMI 0/1 and the jack, volume and output as mixer controls), `rpi_mmal` (Media Kit decoder add-on on the firmware's H.264 decoder), and `rpi_hevc`: a driver for the SoC's own HEVC block and an add-on that does the parsing, picture order and references the block leaves to software (Main and Main 10 up to 4K). Both decoders' pictures match FFmpeg's byte for byte (H.264: 90 pictures of 1080p; HEVC: 25 x265 streams from 176x144 to 4K, eight and ten bit). airTime 1.0.0-14 plays 1080p30 with sound in step: HEVC at 30.0 pictures per second with none left out (software: 19 to 25), H.264 at 29.7 to 29.9 (software: 30.0). A seek far into a group of pictures shows its picture after 1.5 s (was 4.5 s, then seconds of catching up). **Open:** listening to the sound, the jack and HDMI1; the firmware's H.264 decoder slows the ARM's memory down in bursts (5 to 11 pictures left out in 30 s); 4K HEVC decodes but cannot be shown at full rate on the CPU; a video plane would solve both. See `MEDIA.md` | 2026-10-04: `rpi4_tone`, `rpi4_vchiq_audio`, `rpi4_mmal_decode` and `rpi4_hevc_decode ... \| md5sum`, airTime's Stats over telnet, `evidence/airtime-hw1.jpg`, `evidence/airtime-hevc8-hw.jpg`, `evidence/airtime-hevc10-hw.jpg` |
| 9 | Summit with GPU acceleration and WebGL | **Works.** A second arm64 build of Summit's WebKit with Skia, GL compositing and WebGL (`summit_webkit` 1.10.0-2) on Mesa's v3d. get.webgl.org reports WebGL and spins its cube; the WebGL Aquarium runs at 21 fps (500 fish, 1024x1024). **Open:** only those two pages were tried; no measurements against the software engine. See `SUMMIT.md` | 2026-10-04: `evidence/summit-gl1.jpg`, `evidence/summit-aquarium.jpg` |
| 10 | The other applications | **All but Aurora are installed and start.** From `/mnt/HaikuWork/apps`: airShot, Amp (tasamp), Burrow, Kiri, LCDMonitor, Natter, TurboChook, airTime and Summit come as the arm64 packages of the ROCK 5 image; Clipper got an arm64 package (`tools/airos/build-arm64-app-packages.sh ... clipper`). Each was started on the board and stayed running; windows on screen in `evidence/apps1.jpg`. Nothing beyond starting was tested per app. **Open:** Aurora (Electron apps) needs Node.js, which has no arm64 Haiku build | 2026-10-04: launch check over telnet, `evidence/apps1.jpg` |

Also done, outside the stage list:

- **GPIO and AirPins** (2026-10-04): the `rpi_gpio` driver gives programs
  the 40-pin header's GPIO pins (claim as input or output, restored on
  release or close, interrupt-timestamped level changes), and the image
  carries **AirPins**, a GPIO tool after pigg (Andrew Mackenzie,
  Apache-2.0): pigg's board, BCM and compact layouts, inputs with pulls,
  outputs with toggle and hold-to-invert, LEDs and waveforms, pigg's
  `.pigg` files. The driver's self test passes on the board; AirPins was
  driven through the KVM against the driver's state; a card flashed from
  the image with both (sha256 7f900be7…, readback matching) starts AirPins
  from the Deskbar and drives a pin. See `GPIO.md` (`evidence/airpins/`).
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

## Fixed: USB input through the NanoKVM was unreliable

Cause (2026-10-04): the VL805 reads up to four TRBs past the one it executes
and can later use what it read, stale, when another ring comes to use that
memory (Linux: `XHCI_TRB_OVERFETCH` for this chip). This driver puts every
transfer's TRBs in a small chunk of its own from one shared pool, so the TRBs
after a descriptor's Link TRB were other endpoints' descriptors or data
buffers, and the endpoint rings of a device lay back to back. The controller
then ran old contents: an endpoint that stayed idle (cycle bit clear), a "TRB"
error (another transfer type's TRB), transaction errors on the flash drive,
hub control requests that timed out. Three HID interrupt endpoints submitting
small descriptors side by side hit it on almost every boot.

Fix in `xhci.cpp`: four unused TRBs follow every endpoint ring (all
controllers); on the VL805 every descriptor's TRBs are followed by at least
four zeroed ones and the memory stays with its endpoint (a per-endpoint chunk
cache, released when the device goes).

Measured with `tools/rpi4/kvm-hid-probe.sh` (20 reports per function written on
the KVM) 30 s after each start:

| driver | boots | keyboard / mouse / tablet delivered | errors in syslog |
|---|---|---|---|
| before | 3 (2 warm, 1 cold) | 20 / 9-10 / 0-8 | hub timeouts, halted endpoints, transaction and TRB errors on every boot |
| fixed | 8 (5 warm, 3 cold) | 20 / 20 / 20 on every boot | none (only the stalled HID request the KVM does not support) |

End to end on the fixed driver: a command typed through the KVM keyboard ran
in Terminal, and a KVM click opened the Deskbar menu
(`evidence/usbhid-key1.jpg`, `evidence/usbhid-click5.jpg`).

Still open:
- The KVM's absolute pointer is mapped over the whole desktop. With both HDMI
  outputs on (3840 wide) a KVM click lands at twice its x: use
  `nanokvm.py click --width 3840`, or the browser pointer is off.
- The Logitech mouse on the other port enumerates and shows no errors; nobody
  has moved it.

## Fixed: About this system said "ARM Unknown (4 cores)"

Two causes. The shared model table (`headers/private/shared/cpu_type.h`)
only named the RK3588's cores; the Pi 4's MIDR is `0x410fd083`, ARM part
`0xd08`, the Cortex-A72. The table now holds ARM Ltd.'s parts from the
Cortex-A35 to the Cortex-X4. And the firmware's device tree gives the cores
no operating points and no `clock-frequency`, so no speed was shown: the
loader (`platform/rpi/dtb.cpp`) now writes the mailbox's maximum ARM clock
into every `/cpus` node, which the kernel reports as each core's maximum.
The loader prints `ARM clock: up to 1500000000 Hz` on serial.

The same window had no "Processors:" label, no running time and a raw kernel
date: every ICU format failed. The arm64 bootstrap ICU looks for its data in
`/packages/icu74_bootstrap-74.1-1/...`, but the package is installed as
`icu74-74.1_bootstrap-1`. The image now carries the ROCK 5's
`UserSetupEnvironment`, which sets `ICU_DATA=/boot/system/data/icu/74.1`.
Processes outside the desktop session (the system servers) still lack it.

On a card flashed from the image (2026-10-04, sha256 8d152a39…), About this
system opened from the Deskbar shows "4 Processors: ARM Cortex-A72 (4 cores,
up to 1.50 GHz)", "October 3, 2026 at 5:05:09 PM" and "2 minutes, 25
seconds"; `sysinfo -cpu` prints `4 ARM Cortex-A72, revision 410fd083 running
at 1500MHz`. Screenshots and the boot log: `evidence/cpu-name-20261004/`
(`flashed-about.jpg`).

The KVM pointer note above depends on the boot: when HDMI1 is not part of
the desktop (1920 wide), a click needs the default `--width 1920`. With both
outputs on, HDMI0 was the left half on this card, so the Deskbar's menu is
at x 1850 with `--width 3840`.
