# ARM64 accessed-page aging and mapping records

The first installed `+174` SSD trial reached the desktop and passed its
preflight, profiler, 280-child ASID pool and process-teardown checks. During
subsequent SSD read verification, the startup observer's shell on CPU 7
panicked in `VMTranslationMap::PageUnmapped`: the physical page had no matching
page-area record. The call came through a copy-on-write fault. No USB checksum
errors were captured. Automatic recovery returned Linux; independent eMMC
files, filesystem, reference regions and recovery-image integrity passed.
This SSD trial remains failed; `+156` is still the qualified installed baseline.

The ARM64 aging routine used `FlushVAIfAccessed()`'s result as the page's
accessed state. That helper returns false for a user map without an assigned
ASID, and also after flushing a global mapping. When an accessed page was
being considered for unmapping, the routine had retained its PTE, clearing
only accessed/dirty state. The false flush result nevertheless sent it through
`UnaccessedPageUnmapped()`, removing the software mapping record. A later
copy-on-write unmap can then find a valid PTE with no corresponding record.

The correction determines accessed state from the original PTE. Accessed
pages retain their mapping records regardless of whether a TLB invalidation
was needed. Absent mappings return without changing a PTE or removing a
record, and report no modification. Unaccessed valid mappings retain the
existing explicit unmap path.

The host fixture runs the production aging, unmap and invalidation helpers
with architectural substitutes and an independent mapping-lifetime oracle.
The original code reproduces a retained PTE with its record removed for both
an evicted user ASID and a global mapping; a later unmap then fails. Assigned
ASIDs, missing tables/entries, both aging modes and a concurrent hardware
access update are covered. The corrected code passes all 128 host checks.
Three compiled negative controls are rejected: fixing only the global helper's
return value still loses the evicted-ASID record, removing invalidation leaves
stale translations, and removing completion leaves a pending flush.
The native panic is consistent with this defect, but its capture does not
identify the map's ASID or earlier aging operation.

The process probe now optionally holds all children idle before verifying
their private pages over the migration rounds. This gives the page daemon
time to age sleeping maps after identifier recycling. Its bounded idle limit
extends both parent and child deadlines. Six ASan/UBSan host cases cover the
default probe, small/full idle pools, corruption, invalid duration and
interruption during the idle interval; cleanup leaves no children behind.
The planned QEMU and native runs use 280 live children, a 45-second idle
interval and four subsequent migration rounds. Those new kernel/image gates
are not yet fully qualified; elapsed idle time alone is not a count of
page-aging operations. The `+178` build and the two-boot EL2 QEMU run pass,
including both idle pools, memory, storage, profiler and normal reboot checks.
The EL1 run passes its first idle pool, then stops after reboot when the
platform probe exits during its fault-recovery stage under profiling, without
its completion marker. No kernel panic is captured. That failure is retained
for diagnosis; native validation and SSD qualification remain pending.

New evidence: `artifacts/qemu-shell/20260914T201513Z-dd060f` (EL2 pass) and
`artifacts/qemu-shell/20260914T201513Z-bfd88b` (EL1 failure). Network stress was
omitted from these runs because the owner deferred network development.

Evidence beneath `/mnt/HaikuWork`:

- Failed installed trial:
  `artifacts/automated-arm64-installed/20260914T105053Z-827837/failure-review.json`.
- UART and commands: `artifacts/interactive/20260914T105125Z-d3687c`.
- Original/corrected host cases and probe cleanup:
  `artifacts/arm64-page-aging/20260914T110019Z-36f36d`.
- Current candidate plan: `state/arm64-page-aging-plan.json`.

This is separate from the earlier live USB-root replacement panic. Its
shutdown-aware recovery correction and clean native witness are recorded in
[RECOVERY-MEDIA.md](RECOVERY-MEDIA.md). [ARM64-ASID.md](ARM64-ASID.md) records
the reserved-zero correction, whose allocation policy is unchanged here.
