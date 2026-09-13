# ROCK 5 ITX Ethernet bring-up

Both onboard RTL8125 controllers now attach and receive interrupts under the
retained EDK2 v1.1 firmware. Each port has passed bounded, checksum-verified
transfers in the `+106` test image: port 0 at a negotiated 2.5 Gbit/s and port 1
at 1 Gbit/s. Port 0 also passed after normal reboot; an initial firmware stall
required one reset and remains unresolved. [NETWORK-DMA.md](NETWORK-DMA.md)
records the prerequisite DMA work and emulated Intel traffic. Full Ethernet
acceptance remains open, and the SSD installation remains at `+94`.

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
formatting.

## Corrected reporting and both-port image

Source `7a3f5c090b6d156172b5a04baae81273f1bd529c` built as `hrev60097+106`
in `artifacts/build-20260913T062714Z.log`. All 78 host checks passed in
`tmp/host-checks-network-media.log`. The private image SHA-256 is
`9af60264c6e96f5566cfa697c56ddcb60111143c1912c32008e5ae8bef9b6b83`.
QEMU `qemu-shell/20260913T062935Z-3f3b4b` passed both boot/transfer/storage gates,
normal reboot and shutdown; packet review counted 39,401 frames and over
16 MiB of TCP payload in each direction on the PCI fixture.

Native session `interactive/20260913T063242Z-e0d8da` initially stalled after the
UEFI v1.1 banner, with a blank display and no Haiku loader marker. Its first
4536 serial bytes and screenshot were preserved in `first-firmware-stall.*` and
`frame-014.jpg`. One controlled reset reached Haiku. This first attempt remains
a firmware/boot failure with an unidentified cause; it does not disappear from
the qualification because the subsequent driver checks passed.

Both successful Haiku boots passed twelve component hashes, including `ifconfig`
and Network preferences, three settings hashes, USB control and the short
eight-worker memory check. The kernel reported port 0's media `0x900825` at
2500000000 bit/s. `ifconfig` and Network preferences displayed
`2.5 GBit, 2500BASE-T`; both graphical views were inspected in `frame-039.jpg`
and `frame-071.jpg`. Port 1 retained its 1 Gbit/s report. Both INTx routes and
ITS1 NVMe initialized on both boots. The normal Haiku reboot completed without
another manual reset, and the first downloaded file retained its checksum.

| Native transfer phase | Evidence under `artifacts/native-ethernet-transfer` |
| --- | --- |
| Port 0, first successful boot | `20260913T064117Z-4ee823` |
| Port 0, after normal reboot | `20260913T064549Z-564aa9` |
| Port 1, same image after normal reboot | `20260913T064906Z-8a926d` |

Each phase passed an 8 MiB round trip with guest and independent workstation
hashes plus truncated-input rejection. The other Ethernet interface was disabled
for each phase; observed source addresses and per-interface byte counters also
identify the traffic path. No error/drop counter grew during transfers. These
are individual-port tests with both cables installed, not simultaneous traffic
or a throughput benchmark. Both ports' error counters were zero throughout
these +106 transfers. The one interface-down receive error belonged to the
earlier +104 session and remains recorded with that separate result.

`state/native-network-media-qualified.json` retains the reviewed checks and
the initial failed firmware attempt. UART capture and ROOBI recovery completed;
NanoKVM did not restart and its watchdog disarmed. The SSD was not mounted or
updated. Simultaneous traffic, static IPv4/IPv6, sustained mixed load, cable
hotplug, error recovery and throughput remain open, along with the earlier
`+98` QEMU USB timeout and `+88` native startup stall.

## Concurrent stream fixture

`rock5_network_probe` transfers at most 512 MiB per connection using 64 KiB
memory buffers. Every word depends on the stream seed and absolute position;
the receiver checks all bytes and acknowledges verification before the sender
reports success. The bounded protocol checks a private fixture token, direction,
length and seed. Socket timeouts and a process deadline limit stalled runs.
Host tests use an independent wire implementation, partial blocks, corruption,
truncation, rejected acknowledgements and invalid headers/lengths. The source
also compiles as C for the recovery Linux compiler.

The optional QEMU `--network-stream --pci-network` gate runs simultaneous send
and receive streams through the emulated Intel interface on both boots. It
checks the native helper hash and preserves the host peer's source/binary hashes.
Reported rates include pattern generation/checking and final acknowledgement;
they are application transfer measurements, not a claim of wire-rate capacity.

Native simultaneous-port trials use separate temporary IPv4 subnets, explicit
source addresses and observed interface counters/packet paths. Source binding
alone cannot prove the egress interface on the shared DHCP subnet:
`TCPEndpoint::_PrepareSendPath()` caches an unconstrained destination route and
does not consult `socket->bound_to_device`. That socket-option gap remains open;
the datalink path's handling of `SO_BINDTODEVICE` does not establish TCP support.
The stream fixture itself does not change TCP routing or either network driver.

## Native concurrent IPv4 and Linux comparison

Source `9dc5aaaa45ebb6fe3394082f056005bda5d9ae6a` built as `hrev60097+108`
in `artifacts/build-20260913T071312Z.log`. All 83 host checks passed in
`tmp/host-checks-network-stream-final.log`. The private image SHA-256 is
`356691e37cefeee53f5d61de996b27af5bfae08e8b8f0bdb8732f19dbafda08d`.
QEMU `qemu-shell/20260913T071458Z-ab0fb4` passed the memory/platform/cache,
USB control/file-transfer, service and NVMe checks, normal reboot and shutdown.
Both boots passed the PCI file round trip and simultaneous memory streams.
The PCI capture contained 78,158 frames and over 32 MiB of TCP payload in each
direction. This change adds the fixture; it does not modify the network drivers.

Native session `interactive/20260913T072325Z-450eb7` booted on its first attempt
and completed one normal Haiku reboot without an additional GPIO reset. Both
boots passed thirteen component hashes, three settings hashes, USB control and
the 64 MiB/eight-worker memory check. Both ports obtained their DHCP addresses
and reported their expected 2.5/1 Gbit/s links. INTx IRQs 277/282 and ITS1 NVMe
initialized on both boots. The framebuffer desktop after reboot was inspected
in `frame-049.jpg`.

For concurrent traffic, port 0 used `10.240.8.2/24` and port 1 used
`10.240.9.2/24`; the workstation temporarily supplied `.1` on each subnet.
Each run started four streams: one send and one receive per physical port.
The receiver checked every byte, including a seven-byte partial final word.
Packet review reconstructed complete TCP sequence coverage and checked both
MAC addresses in both directions. All four streams overlapped in every run.

| Native phase | Bytes per stream | Evidence under `artifacts/native-network-stream` |
| --- | ---: | --- |
| First boot | 32 MiB + 7 | `20260913T072803Z-24fd7c` |
| First boot, larger transfer | 128 MiB + 7 | `20260913T073019Z-f23cfc` |
| After normal reboot, with CPU sample | 128 MiB + 7 | `20260913T073550Z-e7c9ea` |

All twelve streams passed: 1,207,959,636 payload bytes, approximately 1.125 GiB.
Native interface error/drop counters stayed at zero. The three captures had
675,138 frames in total and no capture drops. Per-port counters increased by
the expected bulk traffic, while USB control counters increased by less than
4 KiB in either direction during each measured transfer interval. The first
fixture requested unsupported
`netstat -r`; it returned a usage message without failing the script. That
run's paths are proven by complete packet coverage. Later runs correctly used
`route list`. The review parser was also corrected to distinguish interface
headers from interface names appearing inside that route listing.

ROOBI Linux passed the same four-stream fixture. Its complete 32 MiB capture is
`linux-network-stream/20260913T074331Z-cc3d9c`; its complete 128 MiB capture is
`linux-network-stream/20260913T074911Z-3fbbb7`. The latter contains 40,311 frames
with no capture drops. The comparison below uses the 128 MiB runs and reports
Mbit/s from the ROCK's perspective:

| Port / negotiated link | Haiku receive | Haiku send | Linux receive | Linux send |
| --- | ---: | ---: | ---: | ---: |
| Port 0, new SFP connection / 2.5 Gbit/s | 144.5 | 86.1 | 1416.4 | 2285.2 |
| Port 1, original connection / 1 Gbit/s | 120.5 | 76.7 | 613.9 | 936.7 |

These are short application measurements with simultaneous traffic, not
maximum link-capacity results. The Haiku numbers above are from the rebooted
system with an eight-second `top` sample. Its first-boot runs were much slower:
about 25–37 Mbit/s per stream, including the larger transfer. The cause of this
variation remains open. In the sampled run, the two Realtek interrupt workers
consumed roughly 0.84–0.92 CPU-seconds per second combined; total reported
utilization was 18.8–21.2% across eight CPUs. Their existing shared Giant lock
is a performance-investigation lead, not an established explanation for the
variation or a reason to remove synchronization without a lifecycle audit.

Linux setup initially lacked development headers, and a later source upload
failed once with SSH exit 255. Both failed setup runs and successful cleanup
are retained; neither sent benchmark traffic. Matching development files were
staged under the lab directory without installing system packages. The first
128 MiB Linux run verified all data but its capture socket dropped eight
packets (`20260913T074506Z-b77462`). It remains incomplete capture evidence.
The repeat used a larger private socket buffer and passed complete coverage.
Linux's vendor RX byte counters also produced implausible totals; rates and
traffic amounts come from probe timing and independent TCP coverage. Its
pre-existing five RX drops per interface did not increase during either
fully captured run.

`state/native-network-stream-qualified.json` and `state/linux-network-stream.json`
retain the qualifications, component hashes, raw counters, timings, capture
hashes and limitations. Native serial capture completed without errors and
ROOBI recovered with boot ID `872a1731-3551-46c7-9512-5771585e4c12`. NanoKVM kept
its boot ID and the watchdog disarmed. Temporary addresses and capture processes
were removed. The SSD was not mounted by Haiku or updated and remains at `+94`.

This advances bounded simultaneous static IPv4 acceptance. Throughput,
variation between boots, TCP device binding, IPv6, long mixed load, cable
hotplug, reset/error recovery and ITS0/MSI-X remain open. The earlier `+106`
firmware stall, `+98` QEMU USB timeout and `+88` installed startup stall remain
unresolved; this successful session does not establish that they were fixed.

## ARM64 memory-copy improvement

The generic ARM64 `memcpy` copied whole words only when source and destination
had matching alignment. `rge_newbuf()` offsets received packets by `ETHER_ALIGN`
(two bytes), while the DMA bounce buffer is page aligned. The `+108` kernel's
disassembly confirms that this path copied the payload one byte at a time.

Revision `381da7d16110257e49bb9a89d12073120b1db039` replaces that implementation
in the ARM64 kernel and libroot with bounded, unaligned word copies for Normal
memory. Both built entry points use paired general-register loads/stores for
the 32-byte loop, followed by eight-byte and byte tails. They contain no SIMD
instructions or calls. This is not a Device-memory/register accessor. DMA
attributes, barriers, interrupt handling and driver locks are unchanged.

The `+110` image is pinned in
`network-intx-image/20260913T082216Z-9862a3/manifest.json`, with SHA-256
`d9afd8f5cb720f364fd045addfee4bb908290c6fc70817f61e4153175cf34341`.
Fifteen component hashes now include libroot and the new copy probe. The network
drivers, network stack, benchmark executable and three driver/settings files
match `+108` byte for byte. Build log
`artifacts/build-20260913T082028Z.log` passed.

All 84 host checks pass. The copy test compiles the production algorithm with
address/undefined-behavior sanitizers and checks 51,301 cases: source/destination
alignments, canaries, unchanged source data, return pointers, zero lengths,
packet-size boundaries and protected page edges. The guest probe calls the
actual libroot entry through a volatile function pointer. It passed the same
51,301 cases on both QEMU boots and both native boots. QEMU evidence
`qemu-shell/20260913T082247Z-59a606` also passes the existing memory, platform,
instruction-cache, services, USB transfer, concurrent PCI network, NVMe
persistence, normal reboot and shutdown gates.

Native session `interactive/20260913T082603Z-4a96fc` reached the desktop on its
first attempt and after a normal reboot, with no additional GPIO reset. Both
boots passed component/settings hashes, the memory probe, DHCP and 2.5/1 Gbit/s
link reporting. All four native traffic trials included the same eight-second
CPU sample. Rates below are Mbit/s from the ROCK's perspective:

| Phase / bytes per stream | Port 0 receive | Port 0 send | Port 1 receive | Port 1 send |
| --- | ---: | ---: | ---: | ---: |
| First boot / 32 MiB + 7 | 249.2 | 123.6 | 211.4 | 123.1 |
| First boot / 128 MiB + 7 | 173.9 | 123.2 | 250.8 | 133.4 |
| After reboot / 128 MiB + 7 | 265.3 | 158.3 | 335.6 | 170.3 |
| After reboot / 256 MiB + 7 | 241.2 | 123.0 | 253.1 | 145.3 |

The sixteen streams verified 2,281,701,488 bytes, about 2.125 GiB. Complete TCP
sequence coverage and MAC addresses prove both physical paths in both
directions. All four streams overlapped in every trial; interface errors/drops
and capture drops stayed at zero. The four evidence directories under
`artifacts/native-network-stream` are `20260913T083013Z-e77254`,
`20260913T083107Z-9e7576`, `20260913T083619Z-3c87bc` and
`20260913T083748Z-2c6881`. Together they contain 1,052,351 captured frames.

Compared with the `+108` after-reboot 128 MiB run, the corresponding `+110`
rates are about 1.8–2.8 times higher. These short application measurements still
vary and remain well below the Linux reference; they do not establish maximum
throughput or explain the variation between boots. Another concrete source of
work is `rge_rxeof()` synchronizing the entire 2,046-byte receive allocation even
for short packets. Any narrower copy must validate the descriptor's fragment
length before using it. The shared Giant lock and CPU placement remain further
performance leads.

Because the change also affects storage paths, the native kernel mounted the
SSD BFS volume read-only and verified both existing 2 GiB regions with eight
workers and independent SHA-256 checks, all four 8 MiB guards, and all eleven
installed package hashes. The volume was unmounted afterward. The installed
system remains at `+94`; no SSD update was performed. The complete native
qualification and recovery receipt are in `state/native-arm64-memcpy.json`.
Serial capture completed with 493,192 bytes and no transport errors. ROOBI
recovered with boot ID `27495453-6502-4de4-9d60-2e4d63b6b08b`; NanoKVM retained
its boot ID and the watchdog disarmed. Temporary test addresses and capture
processes were removed.

## Synchronize the validated receive length

Revision `6ffbb12e732b85405f9e02a02d7911b3c5dc5d32` moves receive fragment-length
decoding before payload synchronization. A valid fragment now synchronizes only
its received bytes, including the CRC removed after frame assembly. Zero or
oversized lengths are marked erroneous and cannot become an
exposed mbuf length; those cases retain full-map synchronization before the
existing discard path. Mapping lifetime, barriers, ring refill and interrupt
locking are unchanged.

The [OpenBSD DMA contract](https://man.openbsd.org/bus_dmamap_sync.9#SYNCHRONIZATION)
defines synchronization over a specified offset and size. The
[Linux r8169 receive routine](https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/ethernet/realtek/r8169_main.c)
also synchronizes the received length. Downloaded reference copies and hashes
are retained in `artifacts/rge-rx-reference/20260913T085135Z`.

The new host test compiles the complete production `rge_rxeof()` and its actual
descriptor constants with checked DMA/mbuf substitutes. It verifies exact copy
extent, untouched tails, delivery and CRC removal, invalid-length discard,
recovery on the next packet, fragmented frames, ring wrap, abandoned fragments,
orphan fragments and descriptors still owned by hardware. The substitutes do
not establish native cache coherency. All 85 host checks, the ARM64 build
(`build-20260913T085324Z.log`) and the full QEMU gate passed.

Image `+112` is pinned in
`network-intx-image/20260913T085451Z-34390b/manifest.json`, SHA-256
`52a1359015b4ca46d9ac7d5a1fe7c1b511fb81348b81f04a678fe0af013553be`.
QEMU evidence is `qemu-shell/20260913T085633Z-55d347`. The network stack, benchmark,
other drivers and settings match `+110`. Libroot's only changed section is its
recorded Haiku revision; the memory-copy machine code is identical. During
inspection, an `objcopy --dump-section` command unintentionally rewrote two
local library snapshots by dropping appended resources. Both were restored to
their original hashes from the pinned build/package before native deployment.
The original boot images were unchanged throughout; the restoration receipt is
`artifacts/libroot-reference-restore/20260913T085921Z/restore.json`.

Native session `interactive/20260913T090225Z-82d0ed` passed first boot and normal
reboot without an additional GPIO reset. Both boots passed fifteen component
hashes, settings, the memory/copy probes and DHCP with 2.5/1 Gbit/s links. The
four simultaneous streams used the same sizes, routes and CPU sampling as the
`+110` comparison. Rates are Mbit/s from the ROCK's perspective:

| Phase / bytes per stream | Port 0 receive | Port 0 send | Port 1 receive | Port 1 send |
| --- | ---: | ---: | ---: | ---: |
| First boot / 32 MiB + 7 | 303.5 | 184.9 | 231.0 | 171.4 |
| First boot / 128 MiB + 7 | 317.4 | 180.6 | 237.8 | 141.1 |
| After reboot / 128 MiB + 7 | 288.0 | 170.0 | 220.1 | 159.2 |
| After reboot / 256 MiB + 7 | 313.5 | 187.5 | 298.5 | 192.2 |

All sixteen streams passed, totaling 2,281,701,488 checked bytes. Complete TCP
coverage and the expected MAC addresses establish both physical paths in both
directions; all four streams overlapped in each run. Interface errors/drops and
capture drops stayed at zero across 1,054,784 captured frames. Evidence directories
under `artifacts/native-network-stream` are `20260913T090713Z-b8ee75`,
`20260913T090811Z-867a05`, `20260913T091358Z-74150d` and
`20260913T091500Z-32df40`.

The longer run improved over `+110`'s 241.2/123.0 and 253.1/145.3 Mbit/s
receive/send results. Shorter comparisons are mixed: port 1 received more
slowly in the matching 128 MiB after-reboot run. This establishes less copy
work and bounded correctness, with some measured throughput gains; it does
not establish a uniform speedup, sustained acceptance or Linux parity.
Native malformed-descriptor injection and jumbo-frame traffic were not tested.
Performance variation, CPU placement, shared synchronization, IPv6 and fault
recovery remain open. Full results are in `state/native-rge-receive.json`.
The SSD was not mounted or updated in this session and remains at `+94`.
Serial capture completed with 306,697 bytes and no transport errors. ROOBI
recovered with boot ID `9fbfe8f2-bf3b-48cf-a5bb-14924684269d`; NanoKVM retained
its boot ID and the watchdog disarmed. Temporary test addresses and capture
processes were removed.

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
