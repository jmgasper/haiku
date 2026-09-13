# ROCK 5 ITX Ethernet bring-up

Both onboard RTL8125 controllers now attach and receive interrupts under the
retained EDK2 v1.1 firmware. Port 1 has passed bounded, checksum-verified transfers
at a negotiated 1 Gbit/s link speed, including normal reboot and interface
reopening. [NETWORK-DMA.md](NETWORK-DMA.md) records the prerequisite DMA work
and successful emulated Intel traffic. Full Ethernet acceptance remains open.

## Legacy interrupt candidate

The experimental `bus_managers/pci/intx/v1` module carries a 32-bit vector
without changing `pci_info` or the existing PCI module tables. It resolves the
legacy API's virtual bus numbers to the owning controller. A controller may
advertise a separate provider; its failures never fall back to a truncated
configuration-space interrupt-line byte. Hosts without a provider retain their
existing byte routing and enable behavior.
ARM64 BSD drivers require the new module to load successfully; they fail
initialization if it is unavailable rather than guessing a byte-sized route.

The RK3588 provider requires the existing exact board/firmware/endpoint checks,
the onboard firmware profile, and `legacy_interrupts true` in `rk3588_pcie`.
Only the following routes are accepted:

| EDK2 segment | Local endpoint | Pin | APB base | GIC INTID |
| --- | --- | --- | --- | --- |
| 3 | 01:00.0 | INTA | `0xfe180000` | 277 |
| 4 | 01:00.0 | INTA | `0xfe190000` | 282 |

The provider validates the device tree's named APB range and root `legacy`
interrupt, four-cell level-high SPI specifier, and GICv3 controller address.
The old child interrupt-controller node has an inconsistent edge flag; the
root's named interrupt is also the one used by Linux. The Haiku GICv3 setup
already initializes SPIs as level-sensitive and routes them to CPU 0.

After validation, the provider maps one APB page as Device memory and masks
the four receive pins through `PCIE_CLIENT_INTR_MASK_LEGACY` at offset `0x1c`.
It unmasks INTA only after a driver installs a handler; the other pins remain
masked. It checks mask readback and restores the original receive mask when the
controller is destroyed. This direct-parent route depends on the captured
single-function topology. It does not implement a general interrupt domain,
bridge swizzling, MSI-X through ITS0, clock/PHY setup or iATU reconfiguration.
`PCIE_CLIENT_INTR_EN_LEGACY` is reserved in the TRM. The initial `+102`
candidate incorrectly required a reset-value bit from this register; the board
reads it as zero. The corrected candidate neither reads nor writes it and gates
interrupts through the documented mask register, as the Linux driver does.

The BSD compatibility layer retains the full vector, enables the route after
handler installation, and masks it before handler removal. It initializes the
worker state and resumes a suspended worker during setup-failure cleanup so
joining it cannot deadlock. Failure to mask an established route stops the
kernel before a handler can reference freed device state. The PCI interface
rejects simultaneous managed INTx and configured/enabled MSI; callers serialize
their per-device mode changes, as with the existing MSI API.

RTL8125 now checks and masks its interrupt status in the top half, including a
readback before scheduling the worker. The worker shares the compatibility
layer's Giant lock with interface, timer and task callbacks. Handler publication
follows fallible hardware and ring initialization. The driver logs bounded
interrupt-count thresholds for native evidence. Broader attachment allocation
cleanup, driver unload, reset/hotplug and error recovery remain open.
Each interface now uses its actual device unit when registering its root-device
association; the former hard-coded zero incorrectly associated a second NIC
with the first root. Interface allocation failure is also checked before bus
mastering is enabled.

## DMA parent correction

RTL8125 creates a 32-bit-address parent DMA tag with `BUS_SPACE_UNRESTRICTED`
segments. The first DMA implementation rejected this sentinel, and the driver
ignored that failure, losing its intended address ceiling. The mapper now
accepts unrestricted parent tags and checks segment-array allocation bounds
when a map is actually created. The driver checks parent-tag creation. The host
regression exercises the exact parent/child pattern and rejects a child mapping
above 4 GiB. This correction still needs native evidence.

## Validation status

All 77 host checks passed in `tmp/host-checks-network-intx-final.log`, including the
actual PCI INTx methods, BSD setup/teardown bodies and Realtek top half compiled
with fault-injecting kernel/MMIO substitutes. Checks cover vectors 277/282,
provider failure without byte fallback, enable/disable ordering, MSI conflicts,
failed configuration writes, masking after endpoint configuration becomes
inaccessible, allocation/semaphore/thread/handler failures, and refusing handler
removal when masking fails. The profile tests reject incorrect resources,
specifier flags, truncated vectors and unsupported BDF/pin combinations.
The initialization checks also inject failure of either required PCI module
and verify reference cleanup and rejection of the ARM64 byte fallback.

The complete `hrev60097+102` image built from
`fdcd191d208a49a5cc0ba42425a178a8b738add9` in
`artifacts/build-20260913T051802Z.log`. QEMU session
`qemu-shell/20260913T052005Z-47931a` passed authenticated USB control, memory,
platform, cache, services, USB file transfer, NVMe persistence, normal reboot
and shutdown. An additional Intel PCI NIC passed an 8 MiB round trip before
and after reboot, including truncated-transfer rejection. Each returned file's
size/hash matched independently. Packet capture contains 39,391 frames, with
16,785,440 TCP payload bytes toward the guest and 16,777,622 toward its peer.
Both boots used the new interface's legacy IRQ 35 fallback; the unrelated
RK3588 firmware profile was rejected in QEMU.

That private candidate is retained under
`artifacts/network-intx-image/20260913T051943Z-a430e7`, with SHA-256
`027f72a035c04cbdff95ab15b70e0360fdabddd16556658d8a4c2d891d0c9385`.
Its `qemu-checkpoint.json` retains the reviewed result, packet counts, component
hashes and native limits. The preceding `+101` candidate also passed
QEMU in `qemu-shell/20260913T051454Z-4562e0`; it was superseded before any native
trial by the two-port association/allocation checks. The older `+98` USB timeout
and native `+88` startup stall remain unresolved.

These QEMU checks cannot establish RK3588 cache coherency or physical interrupt
routing. The subsequent native trials below supply separate evidence.
The lab image includes RTL8125, but its default
Samsung-only PCIe profile does not expose the onboard NICs. A native trial must
enable the onboard profile and legacy routes explicitly, keep AHCI blocked,
retain RNDIS control and recovery, check exact component hashes, and distinguish
interrupt arrivals from successful packet traffic. The physical SSD remains on
its accepted `+94` installation with the separately qualified ICU setting.

## First native trial

The first `+102` native boot, `interactive/20260913T053311Z-7bbac3`, reached USB
remote control and initialized Samsung NVMe interrupts through ITS1. The two
NIC roots passed their device-tree resource checks but were rejected before
mask writes because `EN_LEGACY` read zero. Their APB mode was `0x4c`, mask and
status were both zero. This is a driver-validation defect, not evidence that
legacy interrupts are unavailable. The inventory script then stopped on its
unsupported no-argument `mount` command, before component hashes or the memory
probe; those checks are not accepted. The captured failure is retained, and the
reserved-register correction requires a new build and QEMU/native trial.

## Corrected native attachment

Source `87027df9ce515034dd9bb362a392dd4673bdf2a3` built as `hrev60097+104`
in `artifacts/build-20260913T054224Z.log`. All 77 host checks passed in
`tmp/host-checks-network-intx-mask-only.log`. Its private image SHA-256 is
`3e680cbecfa1ed92b163625ccfbc5990274e5461dcb48c6f47d270eaf95c4dcf`.
QEMU `qemu-shell/20260913T054852Z-bb8e6d` passed both boot/transfer/storage gates,
normal reboot and shutdown. Independent packet review counted 39,732 frames,
16,785,440 TCP payload bytes toward the guest and 16,777,622 toward its peer.
`state/network-intx-checkpoint.json` records this newer QEMU result.

Native session `interactive/20260913T055134Z-f7b607` passed both APB profile
checks, masked the receive pins, installed the two handlers and enabled IRQs
277 and 282 with mask readback `0xe`. Both Realtek interfaces attached and logged
interrupt arrival. `/dev/net/rtl8125/1`, MAC `00:e0:4c:68:06:fd`, negotiated
1 Gbit/s and obtained `192.168.1.154` through DHCP. Port 0, MAC ending `fc`,
had no link. Ten component hashes, three settings hashes, a locked 64 MiB
eight-worker memory check, RNDIS control and the desktop passed. Samsung NVMe
continued using ITS1 interrupts; the SSD filesystem was not mounted.

The first transfer test was invalid: its TCP connection reached the workstation
peer and received the transfer header, but creating the guest file failed with
`ENOENT` because `/boot/home/rock5-lab` did not exist. No payload checksum passed.
The session recovered to ROOBI with complete serial capture; the peer was stopped
and its error retained. The command generator now creates and checks that
directory before transferring. `state/native-network-intx-mask-only-first.json`
records the accepted attachment/DHCP checks and failed test separately. Bulk
traffic, the second port's physical link, warm reboot and sustained behavior
were still unqualified at that checkpoint.

## Native port 1 transfer milestone

The repeat session `interactive/20260913T055957Z-afe24b` used the same `+104`
image and passed three 8 MiB round trips with the workstation at `192.168.1.64`:

| Phase | Transfer evidence under `artifacts/native-ethernet-transfer` |
| --- | --- |
| First boot | `20260913T060421Z-b48dbb` |
| After normal Haiku reboot | `20260913T060721Z-ae41db` |
| After interface down/up and DHCP reconfiguration | `20260913T061002Z-7e2d79` |

Each phase verified the downloaded SHA-256 in Haiku, independently verified the
returned file on the workstation, and rejected a truncated input. Peer sockets
accepted the observed native address `192.168.1.154`. Realtek counters increased
by more than 8 MiB each way per phase; USB carried under 64 KiB each way. No
receive/send error or drop counter increased during the transfers. The earlier
downloaded file retained its hash after reboot. Component/settings hashes,
the short memory check and USB control passed on both boots; desktop captures
were inspected. Native logs show both wide IRQ routes on both boots, retained
ITS1 NVMe initialization, and the requested PSCI reset. The SSD filesystem was
not mounted or updated.

The interface cycle cleared `up`, closed/freed and reopened the device, and
regained DHCP and its 1 Gbit/s link. One aggregate receive error appeared during
the down operation and remained unchanged during the final transfer. The
compatibility layer wakes a closing reader with `B_INTERRUPTED`, which the
stack's reader counts as an error; this is a possible explanation, not an
isolated measurement of that counter's source. Driver unload and physical cable
hotplug were not tested by this interface cycle.

`state/native-network-intx-qualified.json` and the session's `qualification.json`
accept this bounded port 1 milestone. Recovery returned ROOBI with boot ID
`57fdd4c0-a448-4b4b-b7c9-4afc3bf44d96`; UART capture completed without errors,
and the unchanged NanoKVM disarmed its watchdog. The observed 1 Gbit/s link speed
is not a throughput benchmark. Static IPv4/IPv6, simultaneous ports, sustained
mixed load, reset/error recovery and 2.5 Gbit/s traffic remain open.

After this run the owner connected port 0 through a 10 GbE copper SFP.
The new Linux reference, `state/linux-ethernet-two-links.json`, records
`enP3p49s0`/MAC ending `fc` at 2500 Mbit/s full duplex with DHCP `192.168.1.144`.
Port 1 remains at 1000 Mbit/s and `192.168.1.154`. The workstation reports a
5000 Mbit/s link. This establishes the fixture's negotiated Linux speed.

The first native port 0 run, `interactive/20260913T061539Z-181bab`, retained
`+104`. Both ports obtained DHCP, then port 1 was brought down to isolate the
test path. Port 0 passed an 8 MiB round trip and truncated-input rejection in
`native-ethernet-transfer/20260913T062138Z-75e67b`; its counters increased by
8,800,792 receive bytes and 8,900,245 transmit bytes, without errors or drops.
USB carried only 520 receive bytes and 1773 transmit bytes. Recovery and serial
capture completed, and the controller watchdog disarmed. The result is indexed
by `state/native-network-port0-initial.json`.

This run exposed an existing media-reporting defect. RTL8125 returned
`0x900825`, the PHY-derived 2500BASE-T/full-duplex/active media value, but
`ifmedia_baudrate()` discarded the extended Ethernet subtype bits and reported
10 Mbit/s. `ifconfig` also masked those bits and lacked 2.5/5 Gbit/s labels.
The correction uses the existing type/subtype helpers, adds the two labels,
and prevents unknown extended subtypes from aliasing generic `auto`. The host
regression compiles the actual baud-rate table/function and ifconfig formatter
with Haiku's media definitions. It checks the captured value, existing and
extended rates, unrelated flags, unknown subtypes, name parsing and wireless
formatting. The corrected image still requires build and QEMU/native gates.

## References

- Rockchip RK3588 TRM v1.0, Part 2 (2022-03-09), PCIe client status, mask and
  enable registers. The pinned PDF/text and hashes are retained under
  `artifacts/reference`.
- [Linux Rockchip PCIe controller](https://github.com/torvalds/linux/blob/master/drivers/pci/controller/dwc/pcie-dw-rockchip.c),
  named legacy parent, level handlers and individual mask/unmask operations.
- Captured EDK2 device tree:
  `artifacts/firmware-trials/20260911T071501Z-93f34c/efi-diagnostic/firmware.dts`.
- [EDK2 RK3588 v1.1 host initialization](https://github.com/edk2-porting/edk2-rk3588/blob/v1.1/edk2-rockchip/Silicon/Rockchip/RK3588/Library/Rk3588PciHostBridgeLib/PciHostBridgeInit.c),
  initial legacy-mask state and retained firmware PCI topology.
