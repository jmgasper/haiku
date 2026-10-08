# USB removal and Ethernet performance, 7 October 2026

This investigation follows the fresh-install changes in [STATUS.md](STATUS.md).
The owner's keyboard still caused a panic when attached after boot and then
removed. The same report asked for lower Ethernet CPU use and steadier Summit
speed tests. Evidence is under
`/mnt/HaikuWork/rpi4/evidence/usb-network-20261007`; it is not committed.

## USB cancellation

Serial captured `USB object did not become idle!` during endpoint teardown.
The pipe had no queued descriptors but retained two references instead of its
one base reference. The xHCI event and finish threads were sleeping. This is
a separate failure from the earlier HID open/free lifetime fix.

`CancelQueuedTransfers()` snapshots each descriptor's Transfer and clears its
pointer, then drops the endpoint lock while stopping the controller. A report
retry can link a new Transfer in that interval. Cancellation subsequently takes
the entire descriptor list, freeing the newly linked descriptor without freeing
its Transfer. The lost Transfer retains the pipe indefinitely.

An endpoint cancellation counter now rejects linking while any cancellation
is in progress, including its callbacks and descriptor disposal. Linking also
rejects a pipe whose USB ID has been retired. Ordinary submission resumes after
cancellation, and overlapping cancellations cannot reopen admission early.

`tools/rpi4/test-xhci-cancel.py` extracts the production cancellation routine
and admission checks into a controlled scheduling fixture. It passes with
ASan/UBSan for ordinary, overlapping, failed-stop, stalled and forced cancellation,
retired pipes and subsequent reuse. Running with `--baseline e1fddb8f6c`
reproduces the leaked reference and LeakSanitizer failure. This is a software
race reproduction, not emulation of the physical keyboard's USB transactions.

The USB candidate reaches the QEMU desktop, passes twenty late keyboard and
twenty tablet attach/input/remove cycles, and accepts input afterward. Its
matching package and FAT boot archive were then installed on the Pi. The initial
package-only upgrade still used the old boot archive and is not fix evidence.

On the patched native boot, four supported NanoKVM HID resets plus initial and
final gadget rebinding completed without the panic. Typed markers were confirmed
after reset cycles 1, 2 and 4; cycle 3's marker was not received. USB transaction
errors and cancellation occurred during removal. The KVM's RNDIS function still
times out on reinitialization, independently of Ethernet.

The attempted full-speed fixture remained **high speed** according to the USB
controller, despite its `max_speed` setting. The log named
`native-usb-fullspeed.log` and its marker names therefore do not establish
full-speed coverage. The original `super-speed` setting and valid nonbootable
mass-storage backing were restored. The owner's physical keyboard sequence
still needs a retest.

## Network measurements and changes

The target is the same Pi 4B revision 1.5 (`c03115`), 4 GB, with gigabit Ethernet.
The host at `192.168.1.64:8766` serves a deterministic byte pattern without
compression. Each LAN trial downloads 512 MiB to `/dev/null`; the server verifies
the Pi's Ethernet source address. CPU figures below are the mean total CPU use
in seconds 5 through 20 of the same 35-second observation, covering sustained
transfer. One fully busy core is 25% on this four-core board.

| Native configuration | Three download results, Mbit/s | Total CPU |
| --- | --- | --- |
| Original GENET driver | 690.3, 697.7, 666.8 | 65.6% |
| Cached buffers and interrupt batching | 737.4, 744.2, 744.9 | 50.9% |
| GENET changes and bounded TCP checks | 755.7, 751.7, 763.9 | 51.4% |

The driver originally copied packets through uncached buffers and requested an
interrupt for each completed packet. The candidate keeps cacheable, aligned DMA
buffers, invalidates RX data before CPU access, and cleans TX data before device
ownership. RX interrupts are batched to eight packets with a roughly 57 us
timeout. The reader drains pending packets with interrupts masked and rechecks
the producer after arming, avoiding a lost wakeup. TX completion interrupts are
armed only when a writer needs ring space. All nine downloads report no
interface errors or drops. Three 32 MiB downloads and seven 32 MiB uploads
match the expected SHA-256, including four uploads overlapping a 1 GiB
download. Thirty LAN pings have no loss and average 0.220 ms. Concurrent
receive/transmit correctness passes, though upload throughput drops under
receive load; that test is not a claim of line-rate bidirectional throughput.

The GENET register configuration follows the local Linux 6.18.52
`drivers/net/ethernet/broadcom/genet/bcmgenet.c` and `.h`: ring 16's timeout uses
1024 cycles of the 125 MHz clock per tick. QEMU checks image boot and generic
regressions; it cannot qualify GENET DMA or interrupts.

## TCP queue checking and Summit

The first Summit fast.com run reached 70–81% total CPU during busy intervals.
A subsequent whole-system profile attributes 11.83 seconds, 63.68% of the
Ethernet consumer thread's samples, to `BufferQueue::Verify()`. That diagnostic
walks every queued TCP segment before and after packet operations. A growing
queue therefore incurs quadratic verification work.

The TCP candidate retains constant-time boundary checks on the normal debug
path. A full scan remains available with `Verify(true)` or
`DEBUG_TCP_BUFFER_QUEUE=2`. The existing queue test now explicitly performs the
full scan after each add/remove operation; an obsolete sequence-number cast in
that test was also repaired. The full image reaches the QEMU desktop, and the
queue test exits successfully there, covering holes, overlaps, duplicates and
partial consumption. The same test also exits successfully on the native Pi.
Combined LAN performance and integrity results are included above.

The profiled original-driver fast.com result was 93 Mbit/s. A separate,
unprofiled driver-only run returned 110 Mbit/s. These are internet/browser
measurements, not the LAN transfer numbers above; the differing profiler use
also prevents treating their ratio as an isolated driver improvement. The
unprofiled combined GENET/TCP run returned only 24 Mbit/s with total CPU
peaking at 90.5%; removing the queue scan did not resolve the browser problem.

A controlled Summit HTTP fixture fetching three 64 MiB responses from the LAN
host reproduced falling throughput: 253.8, 171.3 and 59.4 Mbit/s. During that
profile, 92.2% of the NetworkProcess main thread's CPU samples are attributed
to `SharedBufferBuilder::append(const FragmentedSharedBuffer&)`. Its vector
append reserves exactly the next required capacity, moving previous segment
entries repeatedly. A native benchmark linked to the installed engine confirms
quadratic growth. For 1,024 / 4,096 / 16,384 / 65,536 appended segments, the
installed routine takes 25.2 ms / 406.7 ms / 6.51 s / 192.17 s. Replacing only
the two incremental mapping appends with ordinary, geometrically growing vector
appends takes 0.52 ms / 1.34 ms / 6.41 ms / 27.16 ms. Both runs verify lengths,
cross-segment reads, offsets, snapshot appends and multi-span content. This is
a direct routine benchmark, not a measured browser speedup; the browser build
and comparison remain in progress.

`rpi4_cpu_watch` records per-core activity and the busiest non-idle threads.
`rpi4_screen_capture` captures valid screens as PPM without image translators.
The latter provides visual evidence from HDMI1 while the NanoKVM reports no
HDMI0 input. The original Screenshot CLI crashed during this investigation;
its empty PNG and 1x1 PPM files are not valid screen evidence.
