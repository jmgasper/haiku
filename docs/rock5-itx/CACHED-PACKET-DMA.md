# Optional cached packet DMA

This experiment follows the IRQ-worker packet-copy samples recorded in
[NETWORK-PROFILING.md](NETWORK-PROFILING.md). It is disabled by default.
Native correctness and throughput qualification are pending; the qualified
`+165` USB image and installed `+156` SSD remain the comparison checkpoints.

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
Host substitutes do not establish actual cache or PCIe coherency. Required next
checks are cross-build/disassembly, both policies in QEMU, and guarded native
packet-integrity trials with comparison measurements and recovery checks.
