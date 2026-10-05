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

The complete `hrev60097+92` image from `8343e1b537` also passed two QEMU boots,
seven component hashes, PCI/NVMe checks, USB transfers, normal reboot and
shutdown in `qemu-shell/20260913T011356Z-7f5a14`. Its private image SHA-256 is
`2ccb84f6e3485449b55af770ad4f796d520de5bd5234b65e5bb3955322c38242`.
This establishes a launcher bug and the focused fix. It does not identify the
wait in the stalled native boot. The SSD still uses the original launcher at
the USB qualification checkpoint described below.

The opt-in [observer script](../../tools/rock5-itx/startup_observer.sh) and
[launch job](../../tools/rock5-itx/startup_observer.launch) are installed as
`/boot/home/rock5-lab/boot-state.sh` and
`/boot/system/settings/launch/rock5-boot-state`, respectively. They are excluded
from ordinary images. The job captures five snapshots of thread states and
selected process file descriptors to UART, then exits. Both the original
launcher image and the fixed image passed the observer's two-boot QEMU checks.
The observer files are now installed on the original `+88` SSD, with flushed
readback hashes and zero BFS allocation counters. Native session
`interactive/20260913T014533Z-453fff` completed all five snapshots, reached the
desktop, and passed file/component hashes, both flushed 2 GiB data regions,
guards and all eleven package hashes. The observer files survived reboot.
The original startup stall has not been reproduced by these traced boots. `state/startup-state-observer-plan.json`
records the immutable inputs and every install attempt.

The installation exposed two lab-script issues: the minimum image lacks `sed`,
and interactive Bash consumes tabs in raw here-documents as completion requests.
Failed attempts reset before their final `sync`; three partial files later
read as zeros. Their contents are preserved separately. The successful install
used base64, flushed writes before checking them, and archived the partial files
outside the launch-settings directory. Do not treat unflushed files followed
by a forced reset as a durability test.

The command transport now waits for the normal shell prompt, transfers a
base64-encoded script to a private temporary file, verifies its SHA-256 before
sourcing it, and removes the temporary file on exit. Writes are paced at
512 bytes per 5 ms with TCP_NODELAY to limit bursts into older guest terminal
drivers. Unit checks execute the wrapper and verify that a changed but valid
base64 payload cannot run; all 75 host checks passed.

This replaces the editing-mode experiment: it preserved tabs in QEMU but failed
on a fresh native login. An intermediate unchecked decode also failed on the
third fresh QEMU login; guest-facing TCP captures contained four identical,
valid programs, locating the damage after network reception. The checked
wrapper then rejected a damaged decode before execution. These failed trials
remain in the evidence; they are not startup-stall or storage failures.

The final paced transport passed `qemu-shell/20260913T015634Z-d8bcf9`, including
three extra fresh logins, exact raw-tab and encoded-file hashes, normal reboot
and shutdown. Three fresh native command sessions then passed observer
persistence, large-file/package checks and the ICU probe while the original
hardware guard remained active. The underlying [terminal write defect](TTY.md)
has a separate deterministic reproduction and candidate kernel fix.

The combined launcher/TTY `hrev60097+94` image passed both full QEMU boots and
a native USB boot in `interactive/20260913T022437Z-e73a47`. All eight component
hashes and five startup snapshots passed. The focused launcher probe also ran
on the ROCK: the fixed reader completed while the unrelated executable stayed
alive; clearing close-on-exec deliberately reproduced the inherited pipe and
delayed EOF. The negative case failed the ordinary expectation and passed its
explicit expected-leak check. Three large unpaced native command sessions and
the original SSD's file/package checks passed, followed by SSD unmount and
verified ROOBI recovery. `state/native-tty-fixed.json` contains the scoped
qualification. The installed-system checks below followed this USB gate.

## Installed launcher and terminal fixes

The `+88` to `+94` Installer update passed a full-capacity QEMU rehearsal and
two NVMe-root boots. Its final evidence checker initially compared a requested
22-character emulated serial with the NVMe field's 20-character value. The
original functional tests passed; re-evaluating every final assertion with
that comparison corrected also passed. Both reports are retained in
`qemu-shell/20260913T024538Z-c20a65`.

Native Installer session `interactive/20260913T025330Z-b22da2` then completed
the same package update. All eleven target package hashes matched, both
existing 2 GiB test regions and their guards were intact, and SSD/USB allocation
counters were zero. The existing EFI loader and network overrides were unchanged.
The SSD was unmounted before recovery.

Installed boots `interactive/20260913T031611Z-a505e7` and
`interactive/20260913T032252Z-6d76ed` each reached the desktop and remote shell.
Eight component hashes, observer/probe/profile files, the terminal probe,
five startup snapshots, eight-worker readback of both 2 GiB regions, guards,
eleven package hashes and filesystem checks passed. NVMe used ITS1 MSI-X on
CPU0; interrupts continued during readback without polling fallback. Both
normal reboots returned to ROOBI, UART capture completed without errors, and
the NanoKVM watchdog disarmed without a controller restart.
`state/native-startup-installed-update.json` records this scoped acceptance.
The ICU settings file was added separately after the second read check; its
subsequent inherited-environment boot test is recorded below. These
successful boots do not close the original startup-stall investigation.

## Time preferences

The preceding Installer session captured a process crash in
`BLocaleRoster::GetAvailableTimeZonesWithRegionInfo()`. The function dereferences
the ICU enumeration without checking its returned pointer or error first.
Read-only inspection also found a package-path mismatch: `libicuuc.so.74.1`
expects `/packages/icu74_bootstrap-74.1-1/.self/data/icu/74.1`, which is absent.
The renamed package supplies the data under
`/packages/icu74-74.1_bootstrap-1/.self/data/icu/74.1` and
`/boot/system/data/icu/74.1`. Native and host data-file hashes match.

`tools/rock5-itx/locale_probe` now provides `rock5_icu_data_probe`. It checks
initialization, canonical time-zone enumeration and decimal formatting without
dereferencing failed ICU objects. In `qemu-shell/20260913T012005Z-e7b326`, the
default package path produced `U_FILE_ACCESS_ERROR` and
`U_MISSING_RESOURCE_ERROR`. Setting `ICU_DATA=/boot/system/data/icu/74.1` for
that process produced 468 zones and the expected `1234.5` string. An explicitly
missing directory reproduced the errors. `state/qemu-icu-and-observer-transfer.json`
retains the probe's checksum and results. The same checks passed on the physical
SSD in `interactive/20260913T012748Z-badc26`, with an identical ICU data-file
hash; `state/native-icu-data-probe.json` retains that result. No persistent ICU
setting or library repair has been applied to the physical SSD at this checkpoint.
This desktop defect is separate from the accepted NVMe integrity checks.

The ROCK image profile now includes a conditional
[UserSetupEnvironment](../../tools/rock5-itx/UserSetupEnvironment) candidate.
For the exact renamed ARM64 bootstrap package, it exports the installed ICU
data path when that data exists and the original package path is absent. An
explicit `ICU_DATA` value is preserved. ICU documents that this environment
variable takes precedence over its compiled directory in the
[data-directory selection rules](https://unicode-org.github.io/icu/userguide/icu_data/#icu-data-directory).
This keeps the package workaround in the lab image profile.

The `hrev60097+96` image from `ab7cd74a2e` passed two full QEMU boots in
`qemu-shell/20260913T025821Z-4d5d54`. Each boot inherited the data path, enumerated
468 canonical zones and formatted `1234.5` correctly. Unsetting the variable
reproduced the missing-data error; sourcing the installed environment file
restored operation. Explicit missing and empty values remained unchanged and
produced the expected errors. Time preferences was visually verified on the
first boot. The second screenshot
shows the desktop, while its process listing contains `Time --update`; it does
not establish a second preferences-window pass. The `df` output uses ordinary
`sprintf` formatting and is not evidence of repaired ICU number formatting;
the dedicated probe checks that directly. Component/file hashes,
startup snapshots, filesystem checks,
USB transfer, normal reboot and shutdown passed. The private image SHA-256 is
`b66742ec9b8999ae4ac8704bcc35687fef3d48ca0c97112cd5ef078d7e99d38f`;
`state/qemu-icu-env.json` records the evidence. This workaround does not add RTC
support or repair the locale API's missing error checks.

The physical `+94` SSD reproduced the missing path in a fresh process with
`ICU_DATA` unset. Both default and explicitly unset runs failed; the correct
path passed. After the second installed-system storage check, only the
498-byte environment file was added to the SSD. Its prior absence, package,
probe and data hashes were checked before writing; the staged file was flushed,
hash-checked and renamed, then flushed and checked again. A sourced child
process passed, and BFS allocation counters remained zero.

Subsequent SSD boot `interactive/20260913T033150Z-f33ee2` inherited the correct
path without an explicit command-session export. All five positive/negative
ICU checks passed, together with eight component hashes, eight settings/probe
file hashes, the terminal probe and filesystem checks. Time preferences opened
normally; both its calendar/clock and time-zone tab were inspected, including
expanding Australia's entries. The requested GUI process was present, rather
than merely a background `Time --update` process. Tracker also displayed
formatted modification dates. Five startup snapshots completed without a
process crash or kernel panic. Normal reboot returned to ROOBI; UART capture
and controller recovery passed without a NanoKVM restart.

`state/native-icu-installed-environment.json` records the native acceptance.
The evidence is **`+94` packages plus the identical qualified settings file**;
the complete `+96` USB image has only QEMU qualification. The clock still starts
at 1970, and RTC/NTP, locale API error handling, broader ARM64 package coverage
and the original intermittent startup stall remain separate work.
