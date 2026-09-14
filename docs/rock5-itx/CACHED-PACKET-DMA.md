# Optional cached packet DMA

This experiment follows the IRQ-worker packet-copy samples recorded in
[NETWORK-PROFILING.md](NETWORK-PROFILING.md). It is disabled by default.
The `+168` candidate passes the host checks, cross-build and both policy modes
in QEMU. Four native boots completed all 48 checked network streams, but that
iteration's automatic Linux recovery failed. A repeat with corrected recovery
media handling is pending. The qualified `+165` USB image and installed `+156`
SSD remain the comparison checkpoints.

Set `cached_packet_buffers true` in
`/boot/home/config/settings/kernel/drivers/rtl8125` to select the experiment
for the onboard ports. The same setting in `ipro1000` selects the emulated
Intel fixture. A driver restart or reboot is required. The compatibility
library reads settings before taking Giant for attachment and records the
policy in each map. It logs the selected path at driver initialization.
Missing settings, a missing value, or `false` retain the original path.

Only private, page-aligned packet bounce allocations become Normal Write-back
memory. Coherent descriptor allocations remain Normal Non-cacheable. The
contiguous-allocation registry records cacheability and excludes cached memory
from coherent direct mapping. Packet load and synchronization still require
neither allocation nor VM lookup while driver locks are held.

For PREWRITE, the mapper copies the payload before cleaning its cache lines to
the point of coherency. PREREAD cleans and invalidates before device ownership,
including combined PREWRITE/PREREAD. After observing completion, POSTREAD
invalidates without cleaning, completes maintenance, then copies received
bytes. POSTWRITE and descriptor synchronization retain their ordering barriers.
All partial boundary lines belong to the private allocation, including page
padding. No unrelated mbuf or descriptor shares those lines. Interrupt masking
prevents migration while using the current CPU's CTR-derived line size; ARM64's
full-memory barrier is DSB SY. Cache lines larger than the owned page alignment
are rejected. These operations follow the streaming-DMA ownership requirements
described in the [Linux DMA guide](https://github.com/torvalds/linux/blob/v6.12/Documentation/core-api/dma-api-howto.rst).

The production mapper's host fixture has independent CPU-cache and device-RAM
views, with cache effects pending until a completion barrier. Tests cover both
policies, allocation failures, descriptor exclusion, mbuf chains, partial ranges,
zero lengths, line sizes from 4 to 4096 bytes, and map reuse. A separate fixture
compiles the production contiguous allocator to test memory policy, address
bounds, coherent lookup, zeroing and cleanup failures. Both run with ASan/UBSan
and with ARM64 noncoherent DMA enabled and disabled.

Five deliberately broken local variants are rejected: invalidating PREWRITE,
cleaning POSTREAD, omitting cache completion, caching descriptors, and accepting
cached allocations as coherent. Evidence is under
`artifacts/cached-packet-dma/20260914T064204Z-d648d3` on the lab drive.
All 115 host checks passed. Disassembly of both actual ARM64 drivers contains
the expected cache instructions and completion barriers. Both policy images
passed two QEMU boots, policy-log and component-hash checks, packet integrity,
profiling, storage read/write, reboot and shutdown checks. The QEMU wrapper's
first run selected a nested MMC transcript incorrectly; correcting its path
selection and repeating the unchanged images passed. Host and QEMU results do
not establish native cache or PCIe coherency.

The first native attempt completed twelve original-policy streams and eight
cached-policy streams. It then lost its USB shell before starting the next
trial. UART recorded EHCI transaction errors affecting RNDIS and HID before
recovery began, without a kernel panic. Haiku labels this error `Device
check-sum error`; the status alone does not establish a physical-wire CRC
failure. The recovery controller returned Linux, whose independent eMMC
filesystem, file and reference-region checks passed. The complete used USB
image was archived and verified before removal from NanoKVM. Its persisted
system log is readable but stops before the error storm captured on UART.

The failure's cause and any connection to cached DMA remain unresolved.
Earlier USB control failures occurred before this change. The repeat used the
same binaries in original/cached/cached/original boot order, with three
four-stream trials per boot. Each stream carried 128 MiB plus seven bytes;
the middle trial used single-PC sampling. All 48 streams passed, verifying
6,442,451,280 payload bytes and 2,884,895 captured frames. Inventory, desktops,
normal reboots, platform checks, sampler teardown and eMMC checks also passed.
No USB transaction errors appeared during the native checks.

Automatic recovery then failed because the writable Linux recovery image's
initrd was overwritten. The [recovery-media investigation](RECOVERY-MEDIA.md)
records the BFS metadata found at the old guest partition offset, the retained
damaged image and the corrected read-only selection. A fresh recovery image
restored Linux; independent eMMC file, filesystem and reference checks passed.
This does not turn the failed automatic recovery into a passing iteration.
The next run repeats the comparison with the corrected controller and requires
the recovery image's original hash after boot before full qualification.
