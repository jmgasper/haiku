# ARM64 BSD network DMA

This is an experimental foundation for the RTL8125 port. The onboard NICs have
not passed a native Haiku traffic test. Interrupt routing remains a separate
prerequisite; Linux's PCI bus numbers and ITS requester IDs must not be assumed
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
`artifacts/network-dma-compile-20260913T040350Z.log`; the complete image and QEMU
qualification are pending. Compilation does not establish device support.

The lab image includes `ipro1000` for QEMU's emulated Intel NIC; `rtl8125` remains
excluded. `qemu_shell.py --pci-network` retains the RNDIS control network and
adds an isolated Intel interface at `10.240.7.15`. Its peer performs an 8 MiB
round trip with independent hashes and a truncated-transfer check, repeated
after a requested normal reboot. The manifest must supply
`network_dma_test.ipro1000_sha256` to identify the exact guest driver. This is a
compatibility-layer test, not a ROCK Ethernet acceptance test.

## Sources

- [FreeBSD bus_dma contract](https://github.com/freebsd/freebsd-src/blob/main/share/man/man9/bus_dma.9),
  particularly static descriptor mappings, synchronization and exclusion bounds.
- This fork's `src/libs/compat/freebsd_network`, OpenBSD compatibility headers
  and `rtl8125/dev/pci/if_rge.c` are the actual consumers reviewed here.
- `state/linux-ethernet-reference.json` records this board's read-only Linux
  inventory and the unresolved interrupt-routing differences.
