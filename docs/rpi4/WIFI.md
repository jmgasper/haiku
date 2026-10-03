# Wi-Fi on the Raspberry Pi 4 (CYW43455)

State 2026-10-03: the onboard Wi-Fi scans, joins a WPA2 network and carries
traffic. Tested on the lab's network ("Gaspers", WPA2-PSK/CCMP, 802.11ac):
DHCP address, ping to the board 0.5/1.2/35.6 ms (min/avg/max of 100), and
with Ethernet taken down the board stayed reachable over Wi-Fi alone and
pulled 8 MB into a RAM disk in 1.0 s (about 66 Mbit/s, checksum matching).

## Pieces

`src/add-ons/kernel/drivers/network/wlan/broadcomfmac`:

- `dev/ic/bwfm.c`, `dev/sdmmc/if_bwfm_sdio.c`: OpenBSD's `bwfm` FullMAC
  driver, on Haiku's OpenBSD compatibility layer like `iaxwifi200`. Changes
  are under `#ifdef __HAIKU__`.
- `host/sdio_host.cpp`: an SDIO host for the BCM2711's first SD controller
  ("mmcnr", Arasan). Haiku's MMC bus has no SDIO (no CMD5/52/53), so the
  driver brings its own: WL_ON through the firmware's GPIO expander, GPIO
  34-39 to ALT3, identification, 4 bit / high speed, CMD52 and CMD53 polled,
  data through the data register. It is built apart from the driver because
  the compatibility headers stand in for the system's.
- `dev/sdmmc/sdmmc_shim.h`: OpenBSD's `sdmmc_io_*` calls on that host.
- `glue.c`: the chip cannot be enumerated, so the driver reports one device
  when the device tree has a BCM2711 (`_fbsd_init_hardware_fixed`, a new
  "fixed" bus type in `libs/compat/freebsd_network`).

Firmware: `brcmfmac43455-sdio.bin`, `.clm_blob` and `.txt` in
`/boot/system/non-packaged/data/firmware/broadcomfmac/`
(`tools/rpi4/fetch-wifi-firmware.sh`; the `rpi4-airos` profile installs
them). The driver is in the regular image's driver list for arm64.

Joining goes the usual way: `ifconfig <device> join <network> <password>` or
the Wi-Fi preferences → net_server → wpa_supplicant → the driver; the
four-way handshake runs in the kernel's OpenBSD net80211.

## What had to change in the port

- The firmware's event structure has a 16 byte interface name; Haiku's
  `IFNAMSIZ` is 32. With the wrong size every scan result was rejected as
  "too small" (and looked almost right: the misplaced length field happened
  to read plausibly).
- OpenBSD's `systq` and the driver's own task queue are one thread in the
  compatibility layer. A command waited on that thread for a response the
  same thread had to fetch, so every command from the driver's task timed
  out. The SDIO bus task now runs on a thread of the driver's.
- The SDIO host raises the card interrupt (the controller's interrupt line,
  shared with the SD card slot's controller) and wakes that thread; the
  interrupt stays off until the thread has served the card.
- A background scan hook, so that the network list can be refreshed while
  associated (net80211 refuses a scan request in RUN without one).
- A detach routine: Haiku unloads the driver when its file is replaced.

## Traps in the lab

- devfs loads anything that appears in a drivers directory. A staged
  `name.new` next to a running driver becomes a second instance on the same
  chip and panics. `tools/rpi4/install-driver.sh` stages elsewhere, moves the
  file and restarts.
- With Ethernet and Wi-Fi on one network the board answers from either
  address.

## Open

- Commands and data are still polled, and data goes word by word through the
  data register (no DMA): 66 Mbit/s received is what that gives. Sending was
  not measured.
- A packaged driver wins over a copy under non-packaged: to try a new build
  on an installed system, block the packaged one in
  `/boot/system/settings/packages` (`BlockedEntries`).
- The Wi-Fi preferences window lists the networks; joining by clicking in it
  was not tried because KVM mouse input is broken on this image (see
  STATUS.md).
- The transmit counter of the interface does not count data frames.
- Only station mode with WPA2-PSK was tried. No 5 GHz/regulatory checks.
- Bluetooth shares the chip and is not up yet (`disable-bt` in config.txt).
