# ARM64 empty-table ASID and process-map reuse

The ARM64 translation-map allocator now reserves ASID zero for the empty user
page table installed while a kernel team runs. User maps use identifiers 1–255.
Recycling skips the reserved entry, and releasing zero is rejected even when
assertions are disabled. All 119 host checks pass. This candidate still requires a complete image build,
QEMU checks and guarded native qualification.

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
replies identify every completed round. A separate eight-child case deliberately
changes one word and must fail while reaping every child; a subsequent 64-child
case verifies operation after cleanup. The parent has a 90-second deadline,
bounded termination/reaping and an outer command timeout; children also have
independent alarms. The helper requires matching kernel/probe hashes and all
expected exits, rounds and cleanup counts.

Linux host execution verifies the probe's normal and deliberate-corruption
paths with no children left behind. The probe also cross-compiles and links
against the Haiku ARM64 headers and libraries. Actual Haiku execution remains
pending. QEMU and native plans require the live pool, failure cleanup and
subsequent pool on each boot, alongside existing platform, profiler, storage,
network, reboot and recovery checks.

The [Linux v6.12 ARM64 context allocator](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/mm/context.c)
also excludes zero for its reserved TTBR0 context. That is a reference for the
identifier's purpose; no Linux implementation was copied into this change.
The earlier profiler faults have not been attributed specifically to ASID
reuse. [PROFILING.md](PROFILING.md) retains those observations and limits.

Offline evidence is under
`/mnt/HaikuWork/artifacts/arm64-asid-zero/20260914T075211Z-offline`.
The promoted production regression's before/after checks are under
`/mnt/HaikuWork/artifacts/arm64-asid-zero/candidate-20260914T085722Z`.
