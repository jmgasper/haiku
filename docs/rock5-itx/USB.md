# USB on the ROCK 5 ITX (2026-10-03)

## Controllers and ports

| Controller (FDT node) | Haiku | What is behind it |
|---|---|---|
| EHCI0 `usb@fc800000` | `ehci` (FDT) | Terminus FE1.1s hub: the M.2 E-key AX210 Bluetooth, front USB 2 |
| OHCI0 `usb@fc840000` | not attached | companion of EHCI0 (never used: the hub is high speed) |
| EHCI1 `usb@fc880000` | `ehci` (FDT) | one USB 2 port (the NanoKVM in the lab) |
| OHCI1 `usb@fc8c0000` | not attached | companion of EHCI1: a low- or full-speed device plugged straight into that port is not seen yet |
| DWC3 USB3OTG0 `usb@fc000000` | **not attached** | the USB-C port (`usb-role-switch`, FUSB302 Type-C controller, USBDP PHY0) |
| DWC3 USB3OTG1 `usb@fc400000` | `xhci` (FDT), USB 2 only | Genesys GL3523 hub (`05e3:0610` + `05e3:0620`): the four rear USB 3 Type-A ports |
| DWC3 USB3OTG2 `usb@fcd00000` | disabled in the FDT | — |

EDK2 v1.1 powers the PHYs, enables the clocks and puts every DWC3 core in
host mode; as with EHCI, Haiku keeps that configuration
(`src/add-ons/kernel/busses/usb/xhci_fdt.cpp`). The DWC3 registers are read
only after the core's bus clocks are confirmed running in the CRU
(`CLKGATE_CON42`), because an unclocked RK3588 block stalls the bus.

## The rear ports

`xhci_fdt.cpp` attaches to `snps,dwc3` nodes. The cores do not snoop the CPU
caches, so the XHCI class takes all of its DMA memory - rings, contexts,
DCBA, scratchpads, transfer buffers - from an uncached pool, copies physical
(`usb_disk`) transfers through those buffers, and orders cycle bits,
doorbells and event reads with full barriers. DWC_usb3 3.00a cannot disable
a port, so the root hub never asks it to (Linux `quirk-broken-port-ped`).

Verified on the board: the GL3523's USB 2 hub enumerates and a USB mouse
(`046d:c077`) behind it shows up in `/dev/input/mouse/usb`; the board booted
every time across three warm reboots and a hardware reset.

**USB 2 only, for now.** With the GL3523's SuperSpeed half set up, the
system soon stopped taking input and starting programs (logins hung). The
USB 3 ports of these cores are therefore disconnected from their USBDP PHY in
the USB GRF (`0xfd5ac000` + `0x1c`/`0x34`, `0x0188`, as Linux
`rk_udphy_u3_port_disable()` does when the PHY only carries DisplayPort), and
the root hub reports them empty. The hub then runs as a USB 2 hub: every
device in the four ports works, USB 3 devices at high speed.

To try SuperSpeed again, put `dwc3_superspeed true` in
`~/config/settings/kernel/drivers/xhci` and reboot. The USB stack does handle
SuperSpeed hubs now (descriptor `0x2a`, `SET_HUB_DEPTH`; the GL3523 hub
enumerates with four ports); what wedges afterwards has not been found yet.
If the board then stops answering, boot once with xHCI disabled (below) and
remove the setting.

## The USB-C port

Attaching the USB-C core froze the whole system early in the boot in eight
boots out of nine, right after the NVMe drive's first MSI: every CPU idle,
even the kernel's own output stopped. Its role and VBUS belong to the FUSB302
and its SuperSpeed lanes to USBDP PHY0, none of which Haiku drives. Cores
with `usb-role-switch` are not attached until a Type-C driver exists.

## Recovering a board whose USB hangs the boot

The Haiku boot menu can disable one system file for one boot. Over the
NanoKVM: reset, send SPACE every 0.1 s for 40 s, then
`DOWN ENTER` (safe mode options), `DOWN`×6 `ENTER` (Disable system
components), `ENTER` (add-ons), `DOWN`×3 `ENTER` (kernel), `DOWN`×3 `ENTER`
(busses), `DOWN`×5 `ENTER` (usb), `DOWN`×3 `ENTER` (marks `xhci`), `ESC`×5,
`DOWN ENTER` (main menu), `DOWN`×4 `ENTER` (continue booting). The keyboard
works there because EDK2 still drives the NanoKVM's HID device.

## How the hangs were found

A temporary watchdog thread in the xHCI module printed, every 2 s on the
serial console (NanoKVM `/dev/ttyS1`, 1 500 000 baud): what each CPU ran,
whether an xHCI register access was in flight, every blocked kernel thread
with the mutex, condition variable or semaphore it waited on (and the
mutex's holder), the frame-pointer chain of the boot thread, and the load
addresses of all kernel images to resolve it offline with `addr2line`.
`elf_debug_lookup_symbol_address()` cannot be used for this outside the
kernel debugger (it asserts the image lock).
