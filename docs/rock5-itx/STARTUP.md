# Startup investigation

The installed `hrev60097+88` SSD boot stalled in
`interactive/20260913T002814Z-a838e2` after NVMe mount and package-daemon
volume verification, before the desktop and RNDIS shell. ITS1 had delivered
interrupts, and there was no reported panic or NVMe timeout. The keyboard
debugger request produced no response. Recovery and the following SSD boot
succeeded, including all post-TRIM data/package checks. The stalled boot's
cause is unknown; `state/native-its-installed-boot-stall.json` retains it.

## Launcher environment pipe inheritance

Source review found that `BaseJob::_GetSourceFileEnvironment()` created its
capture pipe with `pipe()`. An unrelated executable launched by the same
process can inherit the write end. The reader then waits for EOF until that
executable closes it or exits. A long-lived service can therefore prevent
environment setup from finishing. The candidate changes creation to
`pipe2(O_CLOEXEC)`, setting the flag atomically before another thread can exec.
The environment script still receives its intended stdout/stderr duplicates.

`tools/rock5-itx/launch_probe` compiles the real `BaseJob.cpp` into a focused
ARM64 probe. A link wrapper starts an unrelated executable immediately after
the pipe is created. That child checks whether it inherited the actual pipe
and remains alive on a separate control pipe. The probe checks whether the
environment reader can finish before releasing the child. It also verifies
the parsed value, thread completion and child exit; all waits are bounded.
Unused event helpers are hidden and discarded at link time, without replacing
the tested environment-reader implementation.

The original code reproduced the leak in
`qemu-shell/20260913T005314Z-9796e1`: both descriptors lacked close-on-exec,
the unrelated child inherited the pipe, and the reader only completed after
that child was released. The baseline probe SHA-256 is
`772bb4bd51df6546067a38cbdda82e75df0fc02449d015dacf74d005e1ab5b70`.
Normal reboot and shutdown also passed for that QEMU session.

The fixed probe's SHA-256 is
`b4520cbadee918717ed8994d688d00516e5ec21b3aaabfeb9601e48d7fef285c`.
In `qemu-shell/20260913T005634Z-1d0ab4`, both descriptors had close-on-exec,
the unrelated executable inherited neither, and the reader completed while
the child was still alive. The `--clear-cloexec` control deliberately removes
the flags and reproduces the original leak and delay. The evidence indexes
are `state/qemu-launch-environment-baseline.json` and
`state/qemu-launch-environment-fixed.json`.

The probe target is `rock5_launch_environment_probe`. Run its verified binary
from an authenticated lab session with `/boot/home/rock5-lab` present. A normal
run must succeed; `--clear-cloexec` must return status 1 and report both
inheritance and delay. `--expect-leak` is only for evaluating the original
implementation or the deliberate negative control. The helper is not installed
in ordinary images or launched automatically.

This establishes a launcher bug and the focused fix. It does not identify the
wait in the stalled native boot. A separate, opt-in system launch job is
prepared to capture early thread states and selected process file descriptors
to UART; it must pass QEMU validation before deployment. The installed SSD
has not yet received the launcher fix or that observer.

## Time preferences

The preceding Installer session captured a process crash in
`BLocaleRoster::GetAvailableTimeZonesWithRegionInfo()`. The function dereferences
the ICU enumeration without checking its returned pointer or error first.
Read-only inspection also found a package-path mismatch: `libicuuc.so.74.1`
expects `/packages/icu74_bootstrap-74.1-1/.self/data/icu/74.1`, which is absent.
The renamed package supplies the data under
`/packages/icu74-74.1_bootstrap-1/.self/data/icu/74.1` and
`/boot/system/data/icu/74.1`. Native and host data-file hashes match.

`state/time-preferences-observation.json` retains that evidence. A controlled
ICU/API reproduction and repair are pending; this desktop defect is separate
from the accepted NVMe integrity checks.
