# air/OS on the Raspberry Pi 4: lab setup

This is the plan the port started from (revision 2, 2026-10-03), kept for its
reasoning. What was actually built and verified is in STATUS.md and BOOT.md;
where they differ, they are right. Known differences from the plan:

- The NanoKVM is the ROCK 5's former one (192.168.1.8), and the smart plug is
  `switch.attic_rack_power`.
- The recovery OS is Alpine Linux running from RAM (384 MiB image), not
  Raspberry Pi OS: the NanoKVM had no room for more.
- `total_mem=3072` is not set; nothing needed it so far.
- The serial wire has no series resistor that the lab knows of.
- Tools: `tools/rpi4/` (`power.py`, `build-recovery-image.sh`, `deploy-sd.sh`,
  `serial-capture.sh`, `qemu-run.sh`, `shell.py`).

The aim is the same remote loop as the ROCK 5 lab. Build on this host, deploy,
power-cycle the board, then watch serial output and the HDMI picture. Recovery
must work without anyone touching the Pi.

## The setup in one table

| Concern | Choice | Why |
| --- | --- | --- |
| Boot path | Pi EEPROM → `start4.elf` → **our own loader** + a boot archive, both loaded by the firmware | The Raspberry Pi OS way. All boot configuration is plain text the lab can edit. No UEFI settings stored in a file, no ACPI/DT switch |
| Power | Official 5.1 V / 3 A USB-C supply on a **smart plug switched by Home Assistant** | A true cold power cycle through one HTTP call, no soldering |
| Video | **HDMI0** (the micro-HDMI port next to USB-C power) to the NanoKVM's HDMI input | The firmware's frame buffer is on HDMI0. The NanoKVM presents a 1080p sink |
| Keyboard, mouse, virtual disk | NanoKVM HID USB-C to a **black USB 2.0** port | The EEPROM boots from the NanoKVM's virtual disk. air/OS needs its own USB driver before keyboard and mouse work |
| NanoKVM power | Its **own** 5 V supply, **not** on the smart plug | Cutting the Pi's power must not take the KVM down |
| Serial console | Pi GPIO14/15 + GND to the NanoKVM's 3.3 V UART, **115200 8N1**, with a 1 kΩ resistor in the KVM→Pi wire | Until air/OS has USB and Ethernet on the Pi, serial and screenshots are all we have. The resistor stops the KVM half-powering a switched-off Pi |
| Boot modes | EEPROM `BOOT_ORDER=0xf14`: **USB first**, then SD. What is attached on the NanoKVM picks the mode | Recovery never depends on the SD card or air/OS |
| Recovery | Raspberry Pi OS Lite (64-bit) on a NanoKVM virtual disk | Rewrites the SD card over the LAN, even a blank card. Same pattern as the ROCK 5's Debian recovery |
| Fast iteration | A small FAT image (firmware + loader + boot archive) on the NanoKVM, booted by the EEPROM | Loader and kernel changes need no SD write and no air/OS USB driver |
| Storage | microSD: FAT boot partition + BFS | |
| Network | Onboard Ethernet to the LAN with a DHCP reservation | Needed by the recovery OS now, and by air/OS once it has a GENET driver |
| Cooling | Heatsink + always-on 5 V fan on pins 4/6, in an open case | Nothing in air/OS manages the fan. The firmware still throttles on its own |

## How the board boots

```
EEPROM bootloader (SPI flash on the board)
  │  BOOT_ORDER 0xf14: try USB mass storage, then the SD card
  ▼
start4.elf + fixup4.dat (VideoCore firmware, from the chosen FAT partition)
  │  reads config.txt; loads bcm2711-rpi-4-b.dtb + overlays, cmdline.txt,
  │  kernel=<air/OS loader>, initramfs <boot archive> followkernel
  │  fixes up the device tree (memory size, DMA ranges, MAC, serial,
  │  /chosen bootargs + initrd location)
  ▼
built-in armstub: GIC-400, timer frequency; cores 1-3 park on a spin table
  │  jumps to the loader, x0 = device tree
  ▼
air/OS loader (new, see "Fork work")
  │  serial + frame buffer + ARM clock via the firmware mailbox,
  │  memory map from the device tree, kernel + boot modules from the archive,
  │  page tables, start cores 1-3 through the spin table, EL2 → EL1
  ▼
kernel → finds the BFS boot partition on the SD card by the ID in cmdline.txt
```

What we lose compared with UEFI: until air/OS has a USB driver for the Pi, the
NanoKVM keyboard does not work in the boot menu. The menu and safe-mode options
go through serial or `cmdline.txt`; both are easy to automate. The loader also
cannot read USB or SD disks itself. It only uses what the firmware loaded into
memory.

## Power: a smart plug through Home Assistant

Only the Pi's power supply goes on the plug. The NanoKVM and anything else in
the lab stay on unswitched power.

**Calls the lab makes** (Home Assistant REST API, `Authorization: Bearer
<token>`):

| Action | Request |
| --- | --- |
| State | `GET /api/states/<plug entity>` → `"on"` / `"off"` |
| Off | `POST /api/services/switch/turn_off` with `{"entity_id": "<plug entity>"}` |
| On | `POST /api/services/switch/turn_on` with the same body |
| Power draw (if the plug measures it) | `GET /api/states/<power sensor entity>` |

A plug exposed as a `light` or another domain uses that domain's service names
instead. **Power cycle** = off, wait until the state reads `off` (and the
power reading is near 0 W, if available), wait 5 s, then on. A power-measuring
plug is worth having: it shows whether the Pi is drawing power, and a hung
board often draws differently from an idle one.

**Credentials:** the URL and entity IDs go in `rpi4/state/homeassistant.json`.
The token goes in `rpi4/state/homeassistant.token` (mode 600), or in
`HA_TOKEN` in the environment. Neither is ever committed. A Home Assistant
token can switch every device in the house, not just this plug, so make it a
long-lived token for a dedicated non-administrator user if you can. The lab
only ever calls the three requests above. The control script
(`rpi4/tools/power.py status|on|off|cycle`) does not exist yet. It is written
during setup, alongside the existing `nanokvm.py`.

**Things to get right:**

- **Back-powering.** With the plug off, the NanoKVM is still on, and its
  serial TX line idles at 3.3 V into the Pi's RX pin. That can trickle-power the
  Pi through the pin's protection diode, leaving it half-alive, so the next
  power-on is not a clean cold boot. A 1 kΩ resistor in that one wire prevents
  it. **Verify**: with the plug off, both Pi LEDs are completely dark. That
  check also covers the KVM's USB cable.
- **Plug settings:** turn off any auto-off timer or child lock. Set the
  power-on behaviour after a mains outage to "on" (or "previous").
- **Home Assistant is now a lab dependency.** If it is down, the lab cannot
  power-cycle the Pi and has to ask you.
- **Cutting power during an SD write can corrupt the card.** The recovery OS
  syncs and verifies before the lab cuts power. A card that does get corrupted
  is simply rewritten from recovery.

**Optional later:** if Home Assistant proves to be a weak link, J2 (three
unpopulated holes beside the PoE header: GLOBAL_EN, GND, RUN) can be wired to
the NanoKVM's ATX header. That gives a power cycle and a reset that need only
the KVM.

**No PoE HAT:** the official PoE HATs cover the whole 40-pin header with no
pass-through, including the serial pins, and PoE power would bypass the smart
plug. (The Pi firmware drives the HAT's fan over I2C, so the fan should run
without an air/OS driver. Unverified.)

## Wiring

```
Raspberry Pi 4                            NanoKVM
--------------                            -------
GPIO pin 8  (GPIO14 TXD) ---------------> UART RX   (3.3 V)
GPIO pin 10 (GPIO15 RXD) <----[1 kΩ]----- UART TX   (3.3 V)
GPIO pin 6  (GND)        ---------------- UART GND

HDMI0 (micro-HDMI, by USB-C) -----------> HDMI IN
USB 2.0 port (black)     <--------------- HID USB-C (keyboard, mouse, virtual disk, RNDIS)
Ethernet                 ---> LAN         Ethernet ---> LAN

USB-C power <- official PSU <- smart plug (Home Assistant)
                                          KVM power <- its own supply (NOT the smart plug)
```

### Serial console

- Cross TX/RX as drawn and share ground. **Connect no power pin.** Both sides
  are 3.3 V; never use a 5 V adapter on the Pi.
- 115200 8N1, no flow control. The EEPROM (`BOOT_UART=1`) and `start4.elf`
  (`uart_2ndstage=1`) log there, and so will our loader and kernel. One capture
  shows the whole chain.
- `dtoverlay=disable-bt` puts the PL011 (`arm,pl011`, a driver air/OS already
  has) on GPIO14/15 and switches Bluetooth off until air/OS can drive it.
- **On a NanoKVM-PCIe** use UART1 (`/dev/ttyS1`), the header the ROCK 5 uses.
  Only one reader at a time: two readers split the bytes.
- **Known risk:** on the ROCK 5, NanoKVM serial *input* was corrupted at
  1.5 Mbaud. The KVM's UART clock is 25 MHz, which also lands about −3 % off
  115200 unless its UART has a fractional divisor. **Verify** on day one by
  logging into the recovery OS over serial. If input is unreliable, add a
  3.3 V USB-UART adapter (FT232R/CP2102) on this host. Serial input is the only
  way to drive the arm64 kernel debugger.

### HDMI, USB and the NanoKVM's power

- Use a direct micro-HDMI-to-HDMI cable on **HDMI0**. The firmware reads EDID
  at power-on.
- Plug the KVM's HID cable into a **black USB 2.0 port**. The gadget is
  high-speed only, and the blue USB 3 ports stay free for later tests.
- **Power the NanoKVM on its own.** NanoKVM Cube (Full): the AUX USB-C.
  NanoKVM-PCIe outside a PC: its power USB-C, or its PoE module. The NanoKVM
  Lite powers itself from its single USB-C port, which here would be the Pi.
  So it would go down with every power cycle, and it has no UART header; it
  does not fit.

## Firmware and configuration

### EEPROM

The lab board runs the 2022-01-25 EEPROM. Its configuration was updated on
2026-10-06 without changing the firmware version:

```ini
[all]
BOOT_UART=1          # bootloader log on GPIO14/15
BOOT_ORDER=0xf14     # read right to left: USB mass storage, SD card, retry
WAKE_ON_GPIO=1
POWER_OFF_ON_HALT=0
USB_MSD_DISCOVER_TIMEOUT=5000
USB_MSD_LUN_TIMEOUT=500
```

With nothing attached on the NanoKVM, USB boot gives up and the SD card
boots. The default timeouts had cost about 31 seconds before SD selection
with the lab's thumb drive and empty KVM LUN. The shorter timeouts preserve
booting the attached NanoKVM recovery disk; timings and validation are in
`PERFORMANCE.md`. This is a lab EEPROM setting, not part of the SD image.

For a config change, first boot and verify the recovery OS, preserve the
SD FAT partition and current EEPROM configuration, and capture serial. Use
the pinned `rpi-eeprom-config` to put the new configuration into the same
EEPROM version, and `rpi-eeprom-digest` to generate its signature. Stage
`pieeprom.upd`, `pieeprom.sig`, and the pinned `recovery.bin` on the SD FAT
partition. The ROM update reports verification, renames `recovery.bin`
to `RECOVERY.000`, and resets. Check the reported EEPROM configuration from
the recovery OS, remove these three update files, and test both recovery
boot and SD fall-through. Preserve the USB-first order for this lab.

Bootstrap: factory EEPROMs since 2020 default to SD first, then USB. So the
first recovery boot only needs the SD slot empty. An EEPROM from 2019 without
USB boot needs one Raspberry Pi Imager "bootloader" SD card, inserted by hand
once.

### Firmware files

Take `start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb` and the overlays from
the Raspberry Pi firmware repository at a pinned tag (newest today:
`1.20260915`), and record their sha256 in the lab state. Use the Raspberry Pi
DTB that ships with the firmware, the one Raspberry Pi OS uses, so the
firmware's fix-ups apply. Drivers should match compatibles that the mainline
Linux DTB shares, so a later switch stays possible.

### `config.txt`

File names are placeholders until the build rules exist.

```ini
arm_64bit=1
enable_gic=1               # GIC-400 (GICv2); air/OS arm64 already has the driver
enable_uart=1
uart_2ndstage=1            # start4.elf logs on the UART
dtoverlay=disable-bt       # PL011 on GPIO14/15
total_mem=3072             # keep all RAM inside the PCIe DMA window for now
kernel=airos-loader.img
initramfs airos-boot.tgz followkernel
disable_overscan=1
#hdmi_group=1              # only if the picture is not 1920x1080
#hdmi_mode=16
```

`total_mem=3072` keeps all RAM inside the 3 GB window the PCIe controller can
reach, so USB DMA needs no bounce buffers at first. On early boards (B0 chip
revision) the SD controller can only reach the low 1 GB. The SD driver must
follow the device tree's `dma-ranges`, or use programmed I/O to start with.

### `cmdline.txt`

One line of loader and kernel options, for example
`airos.boot=PARTUUID=<disk id>-02 airos.debug=serial` (syntax to be decided).
The firmware adds its own Linux arguments to `bootargs`, so the loader reads
only `airos.*` keys. Safe-mode and blocklist options go here too, so the lab
can change them from the recovery OS without a keyboard.

### SD card layout

| Partition | Type | Contents |
| --- | --- | --- |
| 1, about 256 MiB | MBR type `0x0c`, FAT32 | firmware files, `config.txt`, `cmdline.txt`, the loader, the boot archive |
| 2, rest | `0xeb`, BFS | air/OS |

Use a 32 GB A1/A2 card rated for endurance (the lab rewrites it often), and
keep a spare.

## The deploy and recovery loops

The NanoKVM's attached image picks the boot mode:

| Attached on the NanoKVM | The EEPROM boots | Used for |
| --- | --- | --- |
| `rpi4-recovery.img` | Raspberry Pi OS from USB | rewriting the SD card; reading board facts |
| an air/OS **boot image** (FAT only) | our loader + boot archive from USB; the system from the SD card | loader and kernel iteration, with no SD write |
| nothing | the SD card | normal boots |

**Loop A, loader and kernel (fast):**

1. Build a small FAT image: firmware files, `config.txt`, `cmdline.txt`, the
   loader and the boot archive.
2. Upload it to the NanoKVM (a `.img` name, USB disk mode), attach it, and
   power-cycle through Home Assistant.
3. Capture serial output from power-on and take NanoKVM screenshots.

The archive's kernel must come from the same build as the system package on
the SD card, or carry every module the test needs. Drivers loaded later from
the SD card's packages must match that kernel.

**Loop B, system (BFS changes):**

1. Attach `rpi4-recovery.img` and power-cycle. Raspberry Pi OS comes up on its
   reserved address.
2. Serve the build over HTTP from this host and stream it onto the card
   (`curl … | dd of=/dev/mmcblk0p2 bs=4M conv=fsync`, plus the files on
   partition 1). Read the written extent back and compare sha256, as in the
   ROCK 5 eMMC recipe.
3. Detach the image, power-cycle, and capture.

**Later, once air/OS has Ethernet:** in-place updates over the network, as on
the ROCK 5. Trials then use the firmware's `tryboot` mode. air/OS writes the
trial's files plus a `tryboot.txt` naming them, sets the one-shot tryboot flag
through the firmware mailbox, and reboots. A hung trial is fixed by one power
cycle, which loads the normal `config.txt` again.

The NanoKVM re-applies `/boot/usb.disk0` whenever its gadget rebinds. Keep it
naming the image you mean (a ROCK 5 pitfall). The recovery OS also reports
board facts: RAM size, board revision (B0 or C0 chip), EEPROM version,
temperature and throttling, and the firmware's patched device tree from
`/proc/device-tree`.

## Fork work

| Piece | What it does | Starting point in the tree |
| --- | --- | --- |
| Pi boot platform for the loader | Loaded by `start4.elf` as an arm64 kernel image. Builds the memory map from the device tree (including reserved areas and the spin table). Runs the PL011 console. Gets the frame buffer and the ARM clock (the Pi 4 starts at 600 MHz until the OS raises it) through the VideoCore mailbox. Reads the boot archive from `/chosen`. Builds page tables, starts cores 1–3 through the spin table, drops to EL1 and enters the kernel. Takes options from `bootargs` | EFI arm64 code (MMU, EL2→EL1, spin-table + PSCI SMP, kernel entry); the RISC-V platform (non-EFI, FDT-driven); generic text console and menu; `tarfs`; the old u-boot platform's `/chosen` initrd lookup |
| arm64 boot archive | kernel + boot modules (SD stack, partition maps, BFS, packagefs) as `.tgz` | `FloppyBootImage` / `NetBootArchive` jam rules |
| Boot volume by ID | the kernel picks the SD partition named in `cmdline.txt` | `vfs_boot.cpp` (which already has the ROCK 5's rescan loop) |
| VideoCore mailbox in the kernel | clocks, temperature, VL805 firmware load, tryboot flag | old 32-bit bcm2835 mailbox code |
| Watchdog restart and halt | the kernel only knows PSCI for these, and the Pi firmware has none | Linux `bcm2835_wdt` |
| SD controller (EMMC2) | `brcm,bcm2711-emmc2`; DMA limits from `dma-ranges` | the RK3588 attach in `sdhci_fdt.cpp` (it also insists on a GICv3, which the Pi lacks) |
| PCIe + VL805 xHCI | full link bring-up, since no firmware does it before air/OS. On boards without a VL805 EEPROM, ask the firmware to load the VL805's firmware after reset | Linux `pcie-brcmstb`; the fork's xHCI FDT attach |
| GENET Ethernet | network | FreeBSD `if_genet` through the compat layer |
| Image builder | the FAT boot image, plus full SD images (FAT + BFS) | the `tools/rock5-itx` image scripts |

## Bring-up order

1. **Loader alive:** prints on serial right after `start4.elf` hands over, and
   dumps the memory map, the device tree's key nodes and the archive. (Loop A.)
2. **Loader complete:** frame buffer on HDMI (a NanoKVM screenshot shows the
   boot screen), ARM at full clock, menu over serial.
3. **Kernel starts** on all four cores and stops at "did not find any boot
   partitions". That proves GICv2, PL011, the timer and the spin table.
4. **EMMC2 + boot volume by ID:** desktop from the SD card, display only.
5. **Watchdog:** restart and shut down work.
6. **PCIe + VL805:** NanoKVM keyboard and mouse, USB disks, and the RNDIS lab
   shell, as on the ROCK 5.
7. **GENET:** network, ssh, in-place updates with tryboot trials.

After that: Wi-Fi/BT (CYW43455), audio, the V3D GPU, then the Pi 400, CM4 and
Pi 5.

## Checks before porting starts

1. The NanoKVM is reachable from this host and its screenshot shows the Pi's
   boot screen.
2. `power.py cycle` works 10 times out of 10. With the plug off, the Pi's
   LEDs are fully dark (no back-powering).
3. One cold boot's serial capture shows the EEPROM banner and `start4.elf`
   output.
4. Serial input works: logging into the recovery OS over serial succeeds.
   Otherwise, switch input to a USB-UART adapter.
5. The recovery image boots from the NanoKVM while the SD card is present.
   With nothing attached, the SD card boots. The fall-through time is
   recorded.
6. A minimal FAT boot image on the NanoKVM (firmware files plus a test kernel)
   boots ahead of the SD card.
7. A full SD write from the recovery OS passes a sha256 readback.
8. 10 minutes of load in the recovery OS: `vcgencmd get_throttled` stays `0x0`
   and the temperature settles below 80 °C.

## Decisions and parts for the owner

**Needed from you:**

- **Home Assistant:** its URL, the plug's entity ID (and its power sensor's,
  if it has one), and a long-lived token. A dedicated non-administrator user
  is better.
- **NanoKVM:** which model, its address, `NANOKVM_PASSWORD`, and the lab ssh
  key added for root. It must not be the ROCK 5's (192.168.1.8) or the X399's
  (192.168.1.22). The Cube Full or the PCIe card both work; the Lite does not.
- **Board facts:** RAM size and board revision, if you know them. The
  recovery OS can read both.
- **Disk space:** `/mnt/HaikuWork` is 99 % full (13 GB free; `artifacts/`
  holds 540 GB). Builds and images for the Pi need a few GB, so plan a cleanup
  before porting.

**Parts:**

- A smart plug in Home Assistant (power metering is a plus).
- Official Raspberry Pi 15.3 W USB-C supply.
- Micro-HDMI to HDMI cable.
- Three female-female jumpers and one 1 kΩ resistor.
- 32 GB endurance microSD, plus a spare.
- Heatsink and a 5 V fan, or an open case with a fan that leaves the GPIO
  header reachable.
- Ethernet cable and a DHCP reservation for the Pi.
- A separate 5 V supply for the NanoKVM, unless it runs on PoE.
- Optional: a 3.3 V USB-UART adapter, needed if NanoKVM serial input is
  unreliable.

## Sources

- Raspberry Pi documentation (`raspberrypi/documentation`, develop branch):
  `config_txt/boot.adoc` (kernel, initramfs, armstub, enable_uart,
  disable_poe_fan), `config_txt/memory.adoc` (total_mem),
  `raspberry-pi/eeprom-bootloader.adoc` (BOOT_ORDER, BOOT_UART,
  USB_MSD timeouts), `raspberry-pi/bootflow-eeprom.adoc` (tryboot).
- Raspberry Pi firmware tags, overlay README (`disable-bt`) —
  https://github.com/raspberrypi/firmware
- EEPROM releases — https://github.com/raspberrypi/rpi-eeprom/releases
- Pi 4 boot CPU clock of 600 MHz, raised over the mailbox — Circle docs,
  https://circle-rpi.readthedocs.io/en/44.4/basic-system-services/cpu-clock-rate-management.html
- J2 header (GLOBAL_EN / GND / RUN) —
  https://forums.raspberrypi.com/viewtopic.php?t=243488
- NanoKVM ports, AUX power, PCIe variant —
  https://wiki.sipeed.com/hardware/en/kvm/NanoKVM/quick_start.html,
  https://www.cnx-software.com/2024/12/24/sipeed-nanokvm-pcie-is-an-inexpensive-kvm-over-ip-solution-with-optional-wifi-6-and-poe-support/
- Home Assistant REST API — https://developers.home-assistant.io/docs/api/rest/
- ROCK 5 lab precedents in this fork — `docs/rock5-itx/README.md` (serial),
  `STATUS.md` (UART input corruption), and the EMMC-INSTALL and RECOVERY
  write-ups
