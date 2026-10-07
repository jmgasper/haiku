# Bluetooth on the Raspberry Pi 4 (BCM4345C0)

The installed 2026-10-06 image is checked again for controller startup,
the custom preferences window and LE scanning (18 nearby devices in the
ten-second scan). Pairing, audio and HID profiles remain untested. Evidence:
`evidence/performance-20261006/release/apps-check.txt` and
`bluetooth-preferences.jpg`.

State 2026-10-04: the onboard controller is a local device of the Bluetooth
server. Checked on the board: the Bluetooth preferences shows "Bluetooth is
on - BCM43455 37.4MHz Raspberry Pi 3+-0190 (E4:5F:01:34:72:48)" and lists
nearby devices (`evidence/bluetooth-prefs.jpg`); `bt_le scan` finds a dozen
LE devices; `bt_le services <address> random` connects to one, discovers its
four GATT services over the data channel and disconnects. Pairing a keyboard,
mouse or headset was not tried: nothing in the lab can be put into pairing
mode from here. Classic (BR/EDR) inquiry was not tried separately.

## How it is put together

- The controller hangs on a UART and speaks HCI with H4 framing. It gets the
  **mini UART** (GPIO 30-33, RTS/CTS); the PL011 stays the serial console.
  `config.txt` has `dtoverlay=miniuart-bt` for that (it was `disable-bt`).
- `src/add-ons/kernel/drivers/bluetooth/h4/h4bcm` is the transport: it
  publishes `/dev/bluetooth/h4/h4bcm/0`, which the Bluetooth server finds
  like the USB transports under `h2`. On open it powers the controller
  (BT_ON, the firmware's GPIO expander line 0), resets it, loads the patch
  file `BCM4345C0.hcd` (a list of HCI commands, 323 records) from
  `data/firmware/h4bcm/`, resets it again and writes the board's address,
  then registers with the HCI layer. Received bytes go from the UART
  interrupt into a ring; a thread cuts them into packets.
- The address is the `local-bd-address` the Raspberry Pi firmware writes into
  the device tree. The device manager has no node for the Bluetooth child, so
  the driver reads the flattened tree (`/dev/bus/fdt/blob`).

## Things that cost time

- The mini UART's automatic RTS and CTS are **active high** unless bits 6 and
  7 of its control register are set. With the default the UART tells the
  controller "do not send" forever, and the controller never answers the
  first reset.
- The HCI layer only takes complete packets from a transport, without the H4
  type byte; voice (SCO) packets must be dropped.

## Open

- 115200 baud. The controller can go faster (the device tree says 230400 for
  this UART); that needs the vendor baud rate command and a matching divisor.
- The mini UART's clock is the VPU core clock; the driver reads it once when
  the device is opened. If the firmware changes that clock later, the baud
  rate goes with it (`core_freq` can pin it).
- Pairing and profiles (HID, audio) on this controller are untested.
