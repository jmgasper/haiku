# Partial terminal writes

`tty_write_to_tty_master()` copies the caller's buffer in 256-byte chunks.
If a later chunk cannot be accepted, it previously returned that error even
when earlier chunks had already reached the slave. A nonblocking caller then
sees `EAGAIN` instead of the accepted byte count. Retrying the same input can
duplicate bytes. Haiku's telnet server sets its terminal descriptor nonblocking.

The candidate preserves the accumulated count and returns success when any
earlier chunk was accepted, matching the existing slave-write path. A failure
before accepting any bytes still returns the original error.

`rock5_pty_write_probe` opens a raw, nonblocking PTY pair and writes a 64 KiB
pattern without a concurrent reader. The terminal has a 4 KiB input buffer.
The probe drains the slave and compares the accepted prefix with the return
value. A fixed driver must report each accepted prefix accurately and transfer
all 64 KiB without omission or duplication. Both descriptors are nonblocking;
the outer loop is bounded at 256 iterations. `--expect-partial-error` only
qualifies the known original failure.

Baseline probe SHA-256:
`1858be2808b0f36348b41c65ec02adc0027338b79f76b27796bcad7b36ac2972`.
Original TTY module SHA-256:
`c68ceaddbec2e29561a95fd8d5af1520ed10998d2ac560065de6d9fb1fb23508`.

In `qemu-shell/20260913T020151Z-7b506f`, `write()` returned -1 / `EAGAIN` while
the slave contained the correct first 4,096 bytes. The ordinary probe rejected
that behavior; the expected-error mode confirmed it. Normal reboot, shutdown
and zero BFS allocation counters also passed. `state/qemu-pty-write-baseline.json`
retains the complete result and source/binary hashes.

The physical `+88` SSD reproduced the same result in
`pty-write-build/20260913T020013Z-d64903/native-20260913T020515Z-eadf5b`, using
the same probe and module hashes. `state/native-pty-write-baseline.json`
records that check and the subsequent normal reboot request. The existing
controller watchdog and hardware lock remained active throughout.

This establishes the terminal driver's partial-progress bug. The failed
command transfers had identical valid input in the guest-facing TCP capture;
their damage occurred later. The source bug explains how a nonblocking terminal
writer can repeat accepted bytes, but no internal trace yet ties every damaged
byte in those sessions to that path. The host-side checked and paced transport
remains in use. The kernel fix's full-image and native qualifications are pending.
