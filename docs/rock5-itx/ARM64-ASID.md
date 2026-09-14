# ARM64 empty-table ASID and process-map reuse

The ARM64 translation-map allocator now reserves ASID zero for the empty user
page table installed while a kernel team runs. User maps use identifiers 1–255.
Recycling skips the reserved entry, and releasing zero is rejected even when
assertions are disabled. The `+174` USB image passes all 119 host checks,
complete two-boot QEMU trials at EL1 and EL2, and two guarded native boots with
independent Linux storage verification. The SSD has received the `+174` update,
but its installed qualification is incomplete; `+156` remains the qualified
baseline.

Previously, the first user map could receive ASID zero. Switching to the empty
TTBR0 table retained that identifier without invalidating its old translations.
An independent host TLB model, running the actual production allocator and
switch bodies, reproduces a retained user lookup after that switch. Reserving
only the bitmap bit is insufficient: the old exhaustion loop would dereference
the reserved entry's null user-map pointer. The correction changes all three
parts together. Existing invalidation and completion ordering is retained.

The host fixture substitutes architectural register, TLB, barrier and locking
primitives while retaining the production constructor, destructor, allocation,
release, switch and empty-table install bodies. It covers CnP disabled/enabled,
255 assigned user identifiers, 280 live maps, seven other active CPU maps,
recycling and repeated release/reuse with inactive translations on all eight
modelled CPUs. Deliberately omitting invalidation or its completion barrier is
rejected. The original source and a bitmap-only copy are retained as failing
controls. These checks use ASan/UBSan; they do not establish a native stale read.

The new `rock5_asid_probe` holds 280 forked children alive with different private
page contents at the same virtual address. Four rounds migrate the children
between available CPUs, verify their previous contents, write the next pattern
and check that the parent's page remains unchanged. Per-child gates and atomic
replies identify every completed round. Exit collection releases and reaps at
most sixteen children at a time after all memory rounds complete. A separate
eight-child case deliberately changes one word and must fail while reaping
every child; a subsequent 64-child
case verifies operation after cleanup. The parent has a 90-second deadline,
bounded termination/reaping and an outer command timeout; children also have
independent alarms. The helper requires matching kernel/probe hashes and all
expected exits, rounds and cleanup counts.

Linux host execution verifies the probe's normal and deliberate-corruption
paths with no children left behind. The probe also cross-compiles and links
against the Haiku ARM64 headers and libraries. The initial EL1 and EL2 QEMU
runs completed all 1,120
private-page checks but lost some exit records during the original simultaneous
release of 280 children. A focused diagnostic recorded `waitpid()` returning
`ECHILD`, without a cleanup deadline or memory mismatch. The existing kernel
limits uncollected child-exit records to 32 (`MAX_DEAD_CHILDREN` in
`thread_types.h`), discarding older entries in `thread.cpp`. The probe now
collects small batches while preserving all 280 live maps through the actual
memory checks. That kernel exit-record limit is unchanged. Normal, small and
large injected-corruption host cases and subsequent operation all pass with
bounded cleanup. Both complete QEMU gates subsequently passed with the same
memory workload and all child exits collected normally.

QEMU and native plans require the live pool, failure cleanup and
subsequent pool on each boot, alongside existing platform, profiler, storage,
network, reboot and recovery checks.

## Accepted native evidence

Each native boot completed 280 live children over four migration rounds on all
eight CPUs, deliberate corruption with complete cleanup, and a subsequent
64-child pool over two rounds. Across both boots, 2,496 private-page checks
passed. Sixteen network streams of 128 MiB + 7 bytes each ran in four trials
before and after these pools, with both directions on both ports active together.
All payload, sequence, MAC, concurrency and counter checks passed, with no
capture drops. Both links retained their
2.5/1 Gbit/s negotiation. Cached packet buffers remained disabled.

Exact hashes for 29 components and seven settings, the visible desktop,
platform/memory/copy checks, profiler warmup, 192 profiled fork/exec checks,
48 recovered faults, four SATA-port initialization checks per boot, eMMC
references and files, and normal reboot all passed. Automatic recovery
returned a new Linux boot ID; independent readback matched the full eMMC FAT
partition and three reference regions. The watchdog disarmed, NanoKVM retained
its boot ID, and the complete read-only recovery image retained its original
hash. No USB transaction-error label occurred before the recovery transition.

The transcript validator also accepted five real QEMU/native transcripts and
rejected six modified controls covering leftover/missing children, a missing
round, accepted corruption, a missing kernel-hash result and a wrong CPU count.
This is bounded reuse/migration evidence. It does not prove the old source
produced a stale native read, sustained stability, or SSD qualification.

- Source: `89eba20036a586528249edb76c1d34be0406c9e7`, `hrev60097+174`.
- Image SHA-256: `c2a6951be8b088590f50582dd8d75471635dd4985c8de3d39017ade703b7ff52`.
- Manifest: `artifacts/arm64-asid-image/20260914T091435Z-052ca3/manifest.json`.
- QEMU EL2/EL1: `artifacts/qemu-shell/20260914T091451Z-721889/asid-result.json`
  and `artifacts/qemu-shell/20260914T091451Z-d8450b/asid-result.json`.
- Native qualification: `artifacts/automated-arm64-asid/20260914T092103Z-a69dee/qualification.json`.
- UART, commands and frames: `artifacts/interactive/20260914T092110Z-252b18`.
- Independent Linux files/references: `artifacts/emmc-file-readback/20260914T093358Z-913ebf`
  and `artifacts/emmc-read-reference/20260914T093358Z-e35954`.
- Summary: `state/native-arm64-asid.json`.

The earlier `+172` QEMU failures are retained in
`artifacts/qemu-shell/20260914T090144Z-707736` and
`artifacts/qemu-shell/20260914T090144Z-1a9162`; the focused `+173` exit-record
diagnostic is in `artifacts/qemu-shell/20260914T090832Z-c5c2ee`. The exit-record
review, host cleanup cases and validator controls are under the candidate
evidence directory below.

The [Linux v6.12 ARM64 context allocator](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/mm/context.c)
also excludes zero for its reserved TTBR0 context. That is a reference for the
identifier's purpose; no Linux implementation was copied into this change.
The earlier profiler faults have not been attributed specifically to ASID
reuse. [PROFILING.md](PROFILING.md) retains those observations and limits.

The first `+174` installed SSD trial passed the pool and teardown checks, then
the startup observer's shell hit a missing page-area mapping assertion in
`VMTranslationMap::PageUnmapped`, through a copy-on-write fault, while SSD
verification was running. There were no USB checksum-error labels. Automatic
recovery and independent Linux storage/recovery-image integrity passed. The
trial is retained as failed in
`artifacts/automated-arm64-installed/20260914T105053Z-827837/failure-review.json`.
The ARM64 page-aging path's use of a TLB-flush result as accessed-state evidence
is being investigated separately; the native panic alone does not identify
which map-aging path was taken.

Offline evidence is under
`/mnt/HaikuWork/artifacts/arm64-asid-zero/20260914T075211Z-offline`.
The promoted production regression's before/after checks are under
`/mnt/HaikuWork/artifacts/arm64-asid-zero/candidate-20260914T085722Z`.
