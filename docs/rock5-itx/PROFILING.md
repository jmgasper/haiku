# ARM64 system profiling

The `hrev60097+163` USB image supports bounded system-wide sampling with
Haiku's `profile -a`, including kernel samples. Two native ROCK boots passed
16 sampling checks, profiled process creation/exit and fault recovery, normal
reboot, desktop inspection and recovery to Linux. This establishes a tool for
investigating Ethernet throughput; performance measurements remain a separate
step. The SSD still runs the qualified `+156` installation.

The sampler starts at the saved interrupt PC and follows bounded frame-pointer
chains. It handles the kernel's EL1 and VHE EL2 modes, validates the current
wired kernel-stack bounds, and copies possibly unaligned exception fields.
User reads use the existing fault-safe copy routine with interrupts masked,
preserving an interrupted copy's fault handler and jump buffer. Invalid
addresses, cycles and excessive depth stop the walk. Threads that have moved
to the kernel team during exit no longer contribute stale user frames.

For example, on this candidate's authenticated lab shell:

```sh
profile -a -k -f -s 32 -i 2000 /boot/home/config/non-packaged/bin/rock5_profile_probe user 3
profile -a -k -s 1 -i 2000 /boot/home/config/non-packaged/bin/rock5_profile_probe user 3
```

The first command checks a controlled three-function call chain. The second
samples only the interrupted PC, reducing unwinding work for performance
investigations. Workloads also exercise syscall copies and seven invalid
frame-pointer cases. The host evidence oracle is
[profile_test.py](../../tools/rock5-itx/profile_test.py).

Frame-pointer sampling can omit callers around leaf functions, assembly syscall
stubs, prologues, epilogues and tail calls. The syscall check therefore requires
the observed kernel entry and `main`, while the controlled user workload still
requires all three named functions. This is not DWARF unwinding. The separate
ARM64 debugger stack-walking and interrupt-PC APIs remain unfinished; these
results apply to the system-wide `-a` path. Per-CPU placement and sustained
profiling overhead have not been qualified.

## Failures retained and corrected

An early `+159` QEMU trial exited during the pre-profiling platform checks.
Its cause remains unresolved. Later baseline and repeated platform trials
passed; that does not erase the earlier failure.

Initial kernel sampling rejected saved EL2h frames. Captured exception records
and the EFI handoff showed that VHE keeps this kernel at EL2. Accepting that
mode restored kernel samples. The raw iframe address is the frame pointer;
treating it as the end of an ordinary frame would be incorrect. A later test
controller matched both the main and MMC filesystem `after-reboot.txt` files,
ran four extra passing trials, and incorrectly rejected the count. All 20
separate transcripts were revalidated, preserving the original result. The
current controller matches the complete path.

The first native `+161` trial passed its initial user/kernel sample, then
panicked during user-only sampling at process exit. The interrupted thread had
already moved to the kernel team and empty user map, but retained an EL0 iframe.
Following its old stack caused an address-size fault in `user_memcpy`; debugger
stack printing repeated the fault and eventually exhausted the iframe stack.
Guarded recovery returned the board to Linux, where eMMC integrity passed.

`+162` prevents those stale user reads. Investigation also found that the early
physical allocator reserves memory without clearing it, while the supposedly
empty user page table had never been initialized. `+163` clears every descriptor
and orders those writes before use, and fails if allocation fails. The dirty-page
host regression rejects the old initialization and passes the correction,
including a physical address above 4 GiB. The original native table contents
were not captured, so their contribution to the recorded address-size fault is
an inference. Reserving ASID zero exclusively for the empty table remains a
separate source-review follow-up; this change does not establish ASID isolation.

## Accepted evidence

Source `c1dcd3760c1f1154f2bfb090adc68a71b29b71e6`, image SHA-256
`fd1957384763a3e458cfa0de6a1341dc5a2315cfdf85f04bf7a84971514e356c`.
All 113 host checks and the build passed. Both QEMU modes passed 16 sampling
checks and two profiled process-exit trials, normal reboot and shutdown; the
EL2 run additionally passed the full storage and PCI network regressions.

Both native boots passed 28 component and five setting hashes, memory/copy
checks, platform checks, AHCI attachment on all four direct ports and 2.5/1 Gbit/s
Ethernet links. All 16 probe-thread reports had zero unknown or dropped ticks.
The two profiled process-exit trials completed 192 fork/exec checks and 48
recovered faults in total. Read-only eMMC file/reference checks passed on both
boots. After recovery, Linux independently verified the complete 300 MiB FAT
partition, both fixture files, filesystem consistency and three reference
regions unchanged. The NanoKVM watchdog disarmed and serial capture ended
without transport errors. No physical SATA disk I/O or SSD update is included.

Evidence beneath `/mnt/HaikuWork`:

- Image, snapshots and final qualification:
  `artifacts/arm64-profiler-image/20260914T050045Z-f6eb6a`.
- Host checks, build console, diagnosis and retained early failures:
  `artifacts/arm64-profiler/20260914T034225Z-161834`.
- QEMU EL2 and EL1:
  `artifacts/qemu-shell/20260914T050108Z-bc4391` and
  `artifacts/qemu-shell/20260914T050108Z-53cc95`.
- Native serial/transcripts and inspected desktop frames 007/039:
  `artifacts/interactive/20260914T050820Z-d4ce0a`.
- Automatic cycle and desktop reviews:
  `artifacts/automated-arm64-profiler-exit/20260914T050811Z-6d1d51`.
- Linux FAT and reference readbacks:
  `artifacts/emmc-file-readback/20260914T051818Z-3c5c8e` and
  `artifacts/emmc-read-reference/20260914T051818Z-952de1`.
- Failed native trial: `artifacts/interactive/20260914T044447Z-81fe35`.
- Summary: `state/native-arm64-profiler.json`.
