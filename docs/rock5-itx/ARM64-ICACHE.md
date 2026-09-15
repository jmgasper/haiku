# ARM64 instruction aliases

The first +186 GPU reset image panicked during device discovery, before the
shell or either GPU diagnostic ran. The fault was a load in `get_attr_string`
called by `i2c_elan_support`, on CPU 3. Automatic Linux recovery, the watchdog,
the complete recovery-image hash and independent eMMC integrity checks passed.
The failed image and its exact kernel/module symbols are retained. This boot
does not qualify GPU reset or interrupt delivery.

Disassembly identifies `ldr x19, [x23, #104]` at the fault. The fault address
`0x3f19c368` implies an invalid node pointer `0x3f19c300`. The input probe's
first call should preserve the node argument. The stack trace alone does not
establish where it changed; its printed ARM64 arguments are not register dumps.

## Reproduced cache defect

The translation map synchronizes executable pages through their physical-map
address. The previous `arch_cpu_sync_icache` cleaned data and invalidated
instructions using that same virtual address. On a VIPT instruction cache,
the executable alias can use another cache index and retain stale instructions.
The board has VIPT Cortex-A55 cores 0–3 and PIPT Cortex-A76 cores 4–7, confirmed
by Linux's boot log and per-core `CTR_EL0` readings.

A bounded Linux userspace reference on the actual board maps one anonymous
backing page through a writer and eight executable aliases. It warms each
executable alias, changes only the immediate returned by small functions,
then performs the old Haiku cache-maintenance sequence on the writer. Each
CPU performs 65,536 checked calls. CPUs 0–3 return stale values 32,684, 32,727,
32,761 and 32,718 times respectively; CPUs 4–7 return none. A separate control
invalidating the actual execution address returns the correct values in all
524,288 calls. No GPU, block-device or kernel-module operation is involved.
This reproduces the maintenance defect, without proving it caused the exact
earlier Haiku panic.

## Correction and acceptance

The kernel retains the data clean and completion barrier, then uses
`IC IALLUIS` to invalidate all instruction aliases throughout the
inner-shareable domain, followed by completion and local instruction barriers.
It must include other CPUs' cache types even when the caller runs on a PIPT
core. This conservative operation may cost more than invalidating a known
execution address; performance tuning is separate from correctness.

An independent host cache model runs the production routine with both PIPT
and mixed PIPT/VIPT CPUs, primed aliases, unaligned ranges and page boundaries.
The old routine passes PIPT and fails both mixed cases, including a PIPT
emitter. The correction passes all three cases with ASan/UBSan.
`rock5_cache_probe` now also tests eight executable clones of a writable area,
with alternating writer cores, unchanged mappings between rounds, per-core
execution checks and cleanup. All 133 host checks, the ARM64 build and both
two-boot QEMU modes pass for +187. Each native boot passes 129,024 alias checks
on all eight CPUs with zero mismatches, taking 4,671 and 5,159 microseconds.
The original same-address probe also passes. GPU identity/reset/IRQ checks,
both reviewed desktops, normal reboot/shutdown, recovery and independent
recovery-image/eMMC integrity pass. The exact +186 panic was not deterministically
reproduced; the earlier SSD page-aging and EL1 profiled-fault results still
require their own qualification.

## Sources and local evidence

- [Arm Cortex-A55 r2p0 TRM](https://documentation-service.arm.com/static/5e7e09f6a3736a0d2e862d2f),
  sections A6.3 and B2.32.
- Linux v6.18 [alias synchronization](https://github.com/torvalds/linux/blob/v6.18/arch/arm64/mm/flush.c)
  and [all-cache invalidation](https://github.com/torvalds/linux/blob/v6.18/arch/arm64/include/asm/cacheflush.h)
  provide an independent architecture reference; the Haiku correction and tests
  are independently implemented.
- Failed native trial:
  `artifacts/automated-mali-reset/20260914T232118Z-4e1782/failure-review.json`.
  The used image was archived and verified before its NanoKVM copy was removed:
  `artifacts/nanokvm-image-archive/20260914T233619Z-b70350/result.json`.
- Reproducer, sources, host before/after results and Linux reference:
  `artifacts/arm64-icache-alias/20260914T234133Z-f8fce3` under `/mnt/HaikuWork`.
  The statically linked Linux probe SHA-256 is
  `e0c82e8cd650adb0422653069c3d6e539379027510a2b5c26db0ac4a78c92131`.
- Qualified +187 source `bd41527b86d458cd291fb9cd23b91dbd3a23c379`:
  `artifacts/automated-mali-reset/20260914T235509Z-59b6a7/qualification.json`.
  Image SHA-256
  `9d4fbc5068737d69c17412671941ca7923feff4c865e59616ef30217fc52d4da`.
  QEMU results: `artifacts/qemu-shell/20260914T235228Z-7f6ecd` (EL2) and
  `artifacts/qemu-shell/20260914T235228Z-a3f6eb` (EL1).
