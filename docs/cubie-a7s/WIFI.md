# Wi-Fi and Bluetooth on the Radxa Cubie A7S (AIC8800D80)

The board's wireless module is a Quectel FCU760K, an AICSemi AIC8800D80 on
USB (the A733's USB1 EHCI, behind the onboard CH334F hub). Once the
regulator driver powers it, it shows up as `a69c:8d80`, a ROM loader. The
`aic8800wifi` driver loads the firmware into it, after which it comes back
as `a69c:8d81`: Wi-Fi on a vendor interface, and a standard USB Bluetooth
(H2) controller that `h2generic` drives.

The protocol notes this was written from (the vendor's Linux driver is GPL;
none of its code is used) are in `cubie/evidence/wifi/` on the lab disk:
`DESIGN.md`, `fdrv-protocol.md`, `bt-notes.md`.

## State (2026-10-10)

Measured on the lab's network ("Gaspers", a WPA2-PSK/CCMP mesh; joined on
5 GHz):

- Firmware load 0.31-0.38 s (560 messages); the chip is back as 8d81 0.6-1.4
  s later. The driver attaches during boot.
- Scans of all 38 channels take 1.5 s; the Wi-Fi preferences and the
  Deskbar list the networks.
- Join through net_server and wpa_supplicant (`ifconfig ... join`, the
  Deskbar, or `wifiautojoin` at boot with a saved network): associated, both
  keys installed and the port open in under 2 s, then DHCP. Link VHT MCS 9
  at 40 MHz (HT MCS 7 before VHT was advertised).
- Ping to a LAN host over Wi-Fi: about 1.7 ms, no loss (power save is off).
- 1 GiB from the lab host in 121 s (about 71 Mbit/s, HT40) while a BLE scan
  ran; no USB errors.
- 12 minutes of `ping -i 0.2` over Wi-Fi: 3600 of 3600 answered, median
  1.7 ms (p90 10 ms, p99 31 ms).
- `ifconfig down`/`up` 20 times: rejoins by itself. Reloading the driver
  with the interface down: the chip restarts, the driver attaches again and
  `wifiautojoin` rejoins.
- A firmware restart (forced from the lab with `aicload --reboot`): the
  driver reloads the chip and rejoins 5 s after it left the bus; Bluetooth
  comes back with it.
- Bluetooth: `bt_dev_info` shows the controller (Bluetooth 5.4, HCI/LMP 13,
  manufacturer 0x0b3b, ACL 1021 x 9); `bt_le scan` lists advertisers; the
  BluetoothStatus Deskbar applet shows the adapter.

## Pieces

`src/add-ons/kernel/drivers/network/wlan/aic8800wifi`:

- `host/aic_loader.c`: the ROM loader. It writes the Bluetooth ROM patch
  (with the host's Bluetooth configuration: HCI over USB, not the UART the
  patch table names) and the Wi-Fi firmware, patches the firmware's
  configuration block and starts it: 560 messages, about 0.35 s.
- `host/aic_usb.cpp`: the USB side, plain Haiku code. The driver registers
  for both identities with the USB stack under its own name, so the stack
  loads it again when the chip comes back as the other one. At driver load
  a ROM-mode chip is loaded and the load fails; the next load finds the
  firmware running and attaches. A named kernel area marks a firmware this
  boot loaded: firmware found running without it (a warm restart from
  another system) is sent back to the ROM first. The firmware's message and
  data pipes have one transfer queued each; confirmations are matched on the
  transport's own threads.
- `dev/aic/if_aic.c`: the driver on OpenBSD's net80211, the way
  `broadcomfmac` runs `bwfm`. The firmware scans, associates, encrypts and
  aggregates; net80211 picks the network and runs the WPA handshake;
  wpa_supplicant's `HAIKU_JOIN`, net_server, the Wi-Fi preferences and the
  Deskbar applet work as for any OpenBSD-layer driver. Scan results are the
  beacons the firmware heard, fed to net80211 as received frames. Received
  data arrives as decrypted 802.11 frames that the driver turns into
  Ethernet frames (A-MSDU, and an A-MPDU reorder window of 64 per TID).
- `glue.c`: reports one device when the firmware runs.

Firmware: the files the loader needs, unchanged, with AICSemi's licence
(`LICENSE.aic`) and a provenance note, in
`/boot/system/non-packaged/data/firmware/aic8800wifi/`.
`tools/cubie-a7s/stage-aic8800-firmware.sh` stages them from Radxa's
`aic8800-firmware` package; `tools/cubie-a7s/UserBuildConfig` puts them in
both Cubie images. Before a public release, AICSemi should confirm that
`LICENSE.aic` covers the Wi-Fi images too (it names the Bluetooth files).

Driver settings (`~/config/settings/kernel/drivers/aic8800wifi`):
`debug true` logs what the driver does; `attach_at_boot false` keeps it out
of the first minute after boot (touch its file to bring it in later).

## Bluetooth

Nothing in `h2generic` had to change: it matches the controller by its
interface class and takes its endpoints from interface 0 only. The
Bluetooth ROM patch is part of the Wi-Fi driver's firmware load, so
Bluetooth exists only once `aic8800wifi` has run.

`bluetooth_server` never ran its device manager's looper, so a controller
that appeared after the server had started was never seen; this one
appears during the server's start, and was found on some boots and not on
others. That is fixed in the server.

## Lab tools

- `aicload` (lab images): the loader from userland over usb_raw, and
  `--probe` / `--reboot` for a running firmware.
- `bt_dev_info` (lab images): the controller, its address and versions.
