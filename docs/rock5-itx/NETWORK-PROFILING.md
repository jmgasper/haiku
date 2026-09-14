# Ethernet profiling and open performance findings

Both physical Ethernet links are usable: the newly connected copper SFP path
negotiates 2.5 Gbit/s full duplex on RTL8125 port 0, and port 1 negotiates
1 Gbit/s full duplex. These are negotiated link rates, not measured application
throughput. The workstation peer uses a 5 Gbit/s link.

The `+163` USB candidate passed a paired experiment with four simultaneous,
checked TCP streams: one send and one receive on each port. Each stream carried
128 MiB plus seven bytes; all twelve streams across three trials passed the
receiver's byte-pattern check. Captures verified the complete TCP sequence
coverage and each physical interface's MAC address. Interface errors, interface
drops and capture drops were zero. The experiment transferred 1,610,612,820
checked payload bytes and captured 759,683 frames.

Rates below are Mbit/s from the ROCK's perspective:

| Trial | Port 0 receive | Port 0 send | Port 1 receive | Port 1 send |
| --- | ---: | ---: | ---: | ---: |
| Before sampling | 318.791 | 174.888 | 297.599 | 203.694 |
| Single-PC sampling | 289.953 | 175.029 | 243.880 | 172.283 |
| After sampling | 265.402 | 172.506 | 286.013 | 172.446 |

The sampling command used `profile -a -k -s 1 -i 2000`. Its window includes
process startup and five seconds of settling. Two unprofiled trials do not
establish a causal sampling-overhead estimate, and no per-CPU placement was
recorded. Throughput remains variable and below the recorded Linux reference.

## Findings and limits

The two Realtek interrupt threads contributed 2,080 samples, including 1,121
in `memcpy` (53.9%). This makes payload copying a concrete investigation lead.
The fork already has the ARM64 copy optimization and validated receive-length
copy reduction; these measurements include both. Private DMA bounce buffers
remain noncacheable. Changing that policy requires explicit cache ownership
and coherency tests; descriptor memory has separate requirements.

Sampling while interrupts are masked can bias attribution near lock and
scheduling operations. Samples in `thread_block` or `mutex_unlock` do not
measure time waiting for a lock. The two interrupt threads had zero unknown
ticks but five unresolved image-symbol hits. User samples from newly executed
network probes were partly unresolved, exposing a separate image-notification
bug described in [PROFILING.md](PROFILING.md).

A subsequent `+163` experiment used full stacks, `-a -k -f -s 32 -i 2000`.
Its first unprofiled four-stream trial passed. During sampling, three streams
completed, while the port 0 send did not finish before the 150-second command
deadline. This is retained as a failed experiment.

The capture shows continued TCP progress, with no kernel panic in serial.
After an early retransmission episode, the affected flow sent approximately
24–25 segments per second, each containing 1,448 payload bytes. Late in the
capture, one segment was outstanding at a time and acknowledgments arrived
about 41 ms later. The capture has no sequence gaps, no zero-window event and
no capture drops. It establishes the slow-progress pattern, not its cause.
TCP congestion, retransmission and send-queue state require further diagnosis.

Guarded recovery returned Linux after both experiments. Independent Linux
checks found the complete 300 MiB eMMC FAT partition, both fixture files and
three reference regions unchanged, with a clean filesystem check. The NanoKVM
watchdog disarmed and serial capture ended without transport errors. The SSD
still runs the qualified `+156` installation.

The failed experiment's complete USB image was archived and verified before
removal from NanoKVM. A local-copy BFS extraction found file metadata but only
zero bytes in its profile and workload logs. No final guest sync completed;
those files are not usable profiling evidence. Future experiments must retain
diagnostics before their outer recovery deadline.

## Evidence

All paths are beneath `/mnt/HaikuWork`:

- Accepted paired experiment, qualification and performance review:
  `artifacts/automated-arm64-network-profile/20260914T053004Z-b78b2d`.
- Its native session: `artifacts/interactive/20260914T053013Z-6b58db`.
- Failed full-stack experiment, packet review and recovered image-log review:
  `artifacts/automated-arm64-network-stack-profile/20260914T053942Z-787d14`.
- Its native session: `artifacts/interactive/20260914T053950Z-d7916d`.
- Verified failed-image archive:
  `artifacts/nanokvm-image-archive/20260914T055407Z-c9fc71`.
- Summaries: `state/native-arm64-network-profile.json` and
  `state/native-arm64-network-stack-profile.json`.

Source references for subsequent investigation include the
[Linux v6.12 DMA API guidance](https://github.com/torvalds/linux/blob/v6.12/Documentation/core-api/dma-api-howto.rst),
[ARM64 DMA mapping](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/mm/dma-mapping.c),
[cache maintenance](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/mm/cache.S),
[RFC 5681](https://www.rfc-editor.org/rfc/rfc5681.html) and
[RFC 6582](https://www.rfc-editor.org/rfc/rfc6582.html).
