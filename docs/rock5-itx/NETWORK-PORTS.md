# Rock 5 ITX onboard Ethernet

The EDK2 v1.1 board image now uses the existing `dt-onboard` PCIe profile,
which admits the captured NVMe, SATA, and both RTL8125 endpoints. The profile
enables legacy interrupts for these devices. Both onboard network devices are
present as `/dev/net/rtl8125/0` and `/dev/net/rtl8125/1` and use DHCP.

## Verification on 2026-09-22

* The PCIe profile unit checks passed (3 tests). The full `@rock5full-mmc`
  image built with SHA-256
  `49a3bb2114f921352f49bd56d937a999b429b07525f06f2aa418b23ddecb8044`.
  QEMU reached the Welcome screen; its result is in
  `artifacts/qemu/20260922T122723Z-671fbf/result.json`.
* The same two effective settings were installed in the NVMe system's
  `/boot/home/config/settings/kernel/drivers/rk3588_pcie`. A native reboot
  mounted the NVMe root and attached both RTL8125 controllers, with interrupt
  lines 277 and 282. Each linked at 1 Gbit/s and obtained a
  `192.168.1.*` DHCP lease. The native serial capture is
  `artifacts/network-ports-20260922/native-serial.log`.
* With the other onboard port and USB RNDIS down, port 0 sent and received two
  pings to `1.1.1.1` from its DHCP address `192.168.1.144`; see
  `artifacts/network-ports-20260922/port0-internet-b.jpg`. Repeating the
  isolation for port 1 sent and received two pings to `1.1.1.1` from
  `192.168.1.162`, and resolved and pinged `example.com`; see
  `port1-internet-b.jpg` in the same artifact directory.
* Both onboard ports were restored to automatic configuration. Their final
  leases were `192.168.1.166` and `192.168.1.162` and each answered two LAN
  pings from the development host. The final native interface view is
  `artifacts/network-ports-20260922/final-interfaces-b.jpg`.

DHCP addresses may change on reconfiguration. The interface counters showed
one receive error on each port after the link down/up trials; no packet loss
occurred in the stated pings. This trial checks basic DHCP, Internet, DNS,
and LAN reachability, not sustained throughput or long-term reliability.
