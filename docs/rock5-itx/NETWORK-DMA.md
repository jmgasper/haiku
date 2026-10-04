# ARM64 BSD network DMA

This is an experimental foundation for the RTL8125 port. The onboard NICs have
not passed a native Haiku traffic test. The interrupt candidate is tracked in
[ETHERNET.md](ETHERNET.md); Linux's PCI bus numbers and ITS requester IDs must not be assumed
to match EDK2's retained configuration.

## Implementation

On ARM64 the compatibility allocator gives its private contiguous RAM a Normal
Non-cacheable mapping, using the cache clean/invalidate and mapping transition
already exercised by this fork's NVMe allocator. A leaf spinlock protects the
allocation registry. A separately created OpenBSD descriptor map recognizes
these allocations and keeps them direct; an incompatible tag fails instead of
silently bouncing a descriptor ring.

Ordinary packet buffers use private bounce memory, reserved when the map is
created. Packet load/sync uses the saved physical address and does not acquire
the VM address-space lock. PREWRITE copies CPU data to the device buffer;
POSTREAD copies received data back into existing mbufs without extending the
chain or allocating. The original mapped length is retained independently of
the packet-header length. Drivers must retain the mbuf layout while it is mapped.
All four synchronization operations include ordering for coherent descriptors.

The mapper checks complete address intervals, segment boundaries, map/tag
ownership and loaded state. Allocation or load failures do not publish a loaded
map. OpenBSD wrapper failures release their partial allocations and clear output
handles. ARM64 selects the generic memory-register accessors with hardware
barriers; PCI I/O-port access remains unsupported by those accessors.

This intentionally uses extra memory and packet copies. It does not implement
an IOMMU, arbitrary bus-address translation or a general cache-coherent ARM64
platform policy. Native DMA above 4 GiB, NIC resets, both ports, link changes and
sustained throughput still require separate board evidence.

## Qualification

`tools/rock5-itx/test_bus_dma.py` compiles the actual mapper and OpenBSD wrapper
with host substitutes for kernel allocation, physical memory and barriers. Both
the ordinary and noncoherent paths pass sanitizer checks for allocation failure,
direct descriptor rings, bounced simple and chained packet buffers, empty mbufs,
partial synchronization, changed packet-header length, failed-load reuse, tag
ownership, boundary splitting and 50,000 interval cases checked with wider
integer arithmetic. These tests cannot model real ARM64 cache coherency.

All 76 host checks passed in `tmp/host-checks-network-dma.log`. Both `rtl8125` and
`ipro1000` cross-linked for ARM64 in
`artifacts/network-dma-compile-20260913T040350Z.log`. The final RTL8125 relink,
including the wrapper cleanup and empty-mbuf copy correction, passed in
`artifacts/network-dma-final-rtl8125-20260913T042829Z.log`. The complete `+98`
image built in `artifacts/build-20260913T041514Z.log` from `c68654e89d`.
Compilation does not establish native device support.

The `+98` image used for this checkpoint included `ipro1000` for QEMU's emulated
Intel NIC and excluded `rtl8125`. `qemu_shell.py --pci-network` retains the RNDIS control network and
adds an isolated Intel interface at `10.240.7.15`. Its peer performs an 8 MiB
round trip with independent hashes and a truncated-transfer check, repeated
after a requested normal reboot. The manifest must supply
`network_dma_test.ipro1000_sha256` to identify the exact guest driver. This is a
compatibility-layer test, not a ROCK Ethernet acceptance test. The generic
accessors do not implement PCI I/O ports, including the emulated 82540 driver's
device-local reset path; this fixture does not qualify Intel reset or hotplug.

The first combined run, `qemu-shell/20260913T041631Z-0cca15`, obtained DHCP on the
Intel interface but timed out waiting for a RNDIS control response. USB had no
address and the shell readiness gate failed. That failure is retained in
`state/qemu-network-dma.json`; its cause remains open. The same image without
the extra NIC passed USB/memory/platform/cache/service/file-transfer/NVMe checks,
normal reboot and shutdown in `qemu-shell/20260913T042019Z-12bd62`.

The combined retry in `qemu-shell/20260913T042326Z-f10238` passed those checks plus
an 8 MiB PCI-network round trip before and after normal reboot. The host hash
check path was corrected to the lab image's `system/non-packaged` location;
the guest image and PCI topology were unchanged. Driver SHA-256 was
`036a796d9a52dac7f1f8a59bffee5eb8eaabb04b38a8fbcd7cfa1a101cdbd736`.
Independent packet-capture review counted 39,315 Ethernet frames, with at least
16 MiB of TCP payload in each direction between the PCI interface and its peer.
Normal shutdown and the independent NVMe host readback also passed.

`state/network-dma-checkpoint.json` indexes the reviewed success, original
failure, file hashes and packet counts. The private image is
`lab-shell-images/20260913T041620Z-906837/haiku-lab-shell.img`, SHA-256
`f844f2a0ac56aebea0da519e1ae9b777f6d7c74a3decb90004d5dffca2c6fbc6`.
This establishes the stated QEMU DMA behavior. It does not close the earlier
USB timeout or qualify native RK3588 cache coherency and RTL8125 interrupts.

## Sources

- [FreeBSD bus_dma contract](https://github.com/freebsd/freebsd-src/blob/main/share/man/man9/bus_dma.9),
  particularly static descriptor mappings, synchronization and exclusion bounds.
- This fork's `src/libs/compat/freebsd_network`, OpenBSD compatibility headers
  and `rtl8125/dev/pci/if_rge.c` are the actual consumers reviewed here.
- `state/linux-ethernet-reference.json` records this board's read-only Linux
  inventory and the unresolved interrupt-routing differences.
