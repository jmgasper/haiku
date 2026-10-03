# air/OS on the Raspberry Pi 4: status

Board in the lab: Raspberry Pi 4 Model B rev 1.5, 4 GB (board code c03115),
EEPROM bootloader 2022-01-25, firmware 1.20260915. Branch `rpi4`.

| # | Stage | State | Evidence |
| --- | --- | --- | --- |
| 1 | SD card / boot | **Works on the board.** The minimum image boots from the SD card to the desktop, 1920x1080 on HDMI0, four cores at 1.5 GHz, serial console through loader and kernel | 2026-10-03: KVM screenshot of the desktop; serial capture `evidence/serial/boot3.log` |
| 2 | Ethernet | Driver written (`bcm_genet`), builds, **not run on the board yet** | - |
| 3 | USB | PCIe host bridge driver written (`<pci>bcm2711`, with the VL805 firmware request), builds, **not run on the board yet**. The xHCI driver is the existing PCI one | - |
| 4 | GPU (Vulkan / OpenGL) | Not started | - |
| 5 | Multi-display, Screens preferences | Not started (HDMI0 only, firmware frame buffer) | - |
| 6 | Wi-Fi | Not started | - |
| 7 | Bluetooth / BLE | Not started (the PL011 is the serial console for now) | - |
| 8 | Media decoding, airTime | Not started | - |
| 9 | Summit with GPU and WebGL | Not started | - |
| 10 | The other apps | Not started | - |

Also done, outside the stage list:

- **The full air/OS image** (`jam -q @rpi4-airos build airos-rpi-image`, 4 GB):
  branding, the owner's applications, Summit, firmware packages. Boots to the
  branded desktop in QEMU's raspi4b (2026-10-03, `evidence/qemu-airos.png`);
  not yet written to the board's card.
- **Restart and shutdown** through the BCM2835 watchdog: initializes in QEMU,
  not exercised on the board.
- **Regression check of the shared changes** (fdt `ranges` translation, PL011
  set-up in the kernel, framebuffer driver remap order): an EFI image from
  the same tree boots in QEMU's `virt` machine with a readable serial console
  (2026-10-03).

The lab's NanoKVM stopped answering after a reboot on 2026-10-03 (ping only);
everything marked "not run on the board" waits for it.

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
