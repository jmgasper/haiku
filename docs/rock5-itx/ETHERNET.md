# ROCK 5 ITX Ethernet bring-up

Native RTL8125 traffic is not yet qualified. The two onboard controllers are
identified under the retained EDK2 v1.1 firmware; [NETWORK-DMA.md](NETWORK-DMA.md)
records the prerequisite DMA work and successful emulated Intel traffic.

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
`PCIE_CLIENT_INTR_EN_LEGACY` is inspected but never written.

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

## DMA parent correction

RTL8125 creates a 32-bit-address parent DMA tag with `BUS_SPACE_UNRESTRICTED`
segments. The first DMA implementation rejected this sentinel, and the driver
ignored that failure, losing its intended address ceiling. The mapper now
accepts unrestricted parent tags and checks segment-array allocation bounds
when a map is actually created. The driver checks parent-tag creation. The host
regression exercises the exact parent/child pattern and rejects a child mapping
above 4 GiB. This correction still needs native evidence.

## Validation status

All 77 host checks passed in `tmp/host-checks-network-intx.log`, including the
actual PCI INTx methods, BSD setup/teardown bodies and Realtek top half compiled
with fault-injecting kernel/MMIO substitutes. Checks cover vectors 277/282,
provider failure without byte fallback, enable/disable ordering, MSI conflicts,
failed configuration writes, masking after endpoint configuration becomes
inaccessible, allocation/semaphore/thread/handler failures, and refusing handler
removal when masking fails. The profile tests reject incorrect resources,
specifier flags, truncated vectors and unsupported BDF/pin combinations.

The ARM64 components compile. The complete image and QEMU/native checks are
pending for this candidate. The lab image includes RTL8125, but its default
Samsung-only PCIe profile does not expose the onboard NICs. A native trial must
enable the onboard profile and legacy routes explicitly, keep AHCI blocked,
retain RNDIS control and recovery, check exact component hashes, and distinguish
interrupt arrivals from successful packet traffic. The physical SSD remains on
its accepted `+94` installation with the separately qualified ICU setting.

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
