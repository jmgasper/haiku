# Haiku on the eMMC

The board's soldered eMMC (`Samsung 8GTF4R`, 7,818,182,656 bytes) now carries a
full Haiku install rather than serving only as a secondary device. This page
records the image, how it is written, what had to change in the driver for the
eMMC to be a *boot* device, and what is verified.

## The image

`@rock5full-mmc` (see `tools/rock5-itx/UserBuildConfig`) builds
`haiku-rock5full-mmc.image`: a 32 MiB EFI system partition holding
`/EFI/BOOT/BOOTAA64.EFI` plus a 6144 MiB BFS partition.

The profile name matters. `build/jam/DefaultBuildProfiles` gives
`HAIKU_BUILD_TYPE = regular` to any profile outside `bootstrap-*`, `minimum-*`
and `test-*`, while only `nightly-*` and `release-*` call
`AddHaikuImageSystemPackages`. A name outside both sets therefore selects the
complete regular image definition — every in-tree application, preference and
demo — without requiring the HaikuPorts packages that have no arm64
repository.

Contents: 35 applications, 21 preferences, 10 demos and 13 data translators
from the regular definition, plus, under `system/non-packaged` (packagefs owns
`/boot/system`, so nothing outside a package is visible anywhere else):

| path | contents |
|---|---|
| `lib/` | the fork's Mesa and GLVND, `libGLU`, `libsqlite3`, `libtag`, `libcurl`, `libpcre2-8`, `libscintilla`, `liblexilla` |
| `add-ons/opengl/egl_vendor.d/` | `10_mesa_panfrost.json` |
| `data/firmware/mali/arch10.8/` | `mali_csffw.bin` |
| `demos/` | `GLTeapot`, `Haiku3d` |

The owner's three applications are no longer loose files under
`non-packaged/apps`; they are installed as packages — see
[The owner's applications](#the-owners-applications) below.

`GLTeapot` and `Haiku3d` are filtered out of the regular definition by `@mesa`
because Haiku has no arm64 mesa package; the fork supplies its own OpenGL, so
both are added back explicitly and built against it.

## Why a regular arm64 build needed fixes

Five things in the tree assumed a package set arm64 does not have:

1. `images/definitions/regular` listed `libmidi.so` unconditionally, but
   `src/kits/midi/Jamfile` only builds it when the `fluidlite` build feature is
   available. Now `libmidi.so@fluidlite`, matching `definitions/test`.
2. MidiPlayer, PatchBay and the Media preference link that library; all three
   are filtered `@fluidlite` and guarded in their Jamfiles.
3. `src/add-ons/translators/wonderbrush/Jamfile` put its own `support/`
   directory on `SEARCH_SOURCE` but never on the header path, so `Layer.cpp`
   could not find `bitmap_compression.h`. One `SubDirHdrs` line; the translator
   is in the image.
4. `src/data/package_infos/generic/haiku` required `noto_sans_cjk_jp` and the
   three wifi firmware packages on any regular build, and
5. `cmd:bunzip2`, `cmd:gunzip`, `cmd:tar` and `cmd:unzip`. Both sets are now
   gated on `!defined(HAIKU_PACKAGING_ARCH_arm64)`.

Adding the missing packages to `build/jam/repositories/HaikuPorts/arm64`
instead does **not** work: the remote repository index is fetched by a checksum
of the exact declared package list, so any addition returns 404.

## The display driver

A fresh eMMC install came up with Screen reporting **Framebuffer**. The driver
was not missing: `/dev/graphics/rk3588_display` is published and
`rk3588_display.accelerant` is in `system/non-packaged/add-ons/accelerants`.
What was missing is the opt-in.

`InitDriver()` in `src/add-ons/kernel/drivers/graphics/rk3588_display/driver.cpp`
reads `load_driver_settings("rk3588_display")` and only sets
`accelerantEnabled` when `firmware_profile` names one of the qualified display
stages. Without the file every flag stays false, so `Control()` answers
`B_GET_ACCELERANT_SIGNATURE` with `B_DEV_INVALID_IOCTL`,
`AccelerantHWInterface::_OpenAccelerant()` fails, and
`_OpenGraphicsDevice()` falls through to `/dev/graphics/framebuffer`. The
refusal is deliberate — the other profiles must stay invisible to app_server —
but it is silent, and nothing in the image supplied the file.

The image now ships `home/config/settings/kernel/drivers/rk3588_display`:

```text
firmware_profile rock5-itx-edk2-v1.1-display-cursor-desktop
```

That is the HDMI1 stage: accelerant, mode set and the hardware cursor on the
one connector, which keeps the desktop exactly where the firmware put it.
`rock5-itx-edk2-v1.1-display-dp-span-cursor` is the wider stage-6
configuration (3840x1080 across HDMI1 and DP1, a cursor per port) and is a
one-word change to the same file. [DISPLAY.md](DISPLAY.md) lists every profile.

With the file in place the boot log reads

```text
rk3588_display: validated VOP2 0xfdd90000 and HDMI TX1 0xfdea0000 resources;
    ... accelerant enabled; modeset enabled; cursor enabled; cursor hooks enabled
rk3588_display: frame buffer acquired at 0xe255000 ... retrace=on irq=188
rk3588_display: cursor shown result=0 polls=0 control=0x1
```

and Screen reports **RK3588 VOP2 HDMI TX1 (Rockchip RK3588 VOP2)** on
`VCS Connector 26"` at 1920x1080, 24 bits, 60 Hz.

### The GPU is gated the same way

`mali_csf` reads its own `firmware_profile` and its log line is easy to read
past: `mali_csf: validated firmware resources at 0xfb000000; identity profile
**disabled**`. Everything downstream of `identityEnabled` — reset, firmware,
commands, shaders — stays off, so Mesa finds no usable device and falls back to
the software rasteriser. That is not an error anywhere: GLTeapot renders, just
slowly.

The image therefore also ships
`home/config/settings/kernel/drivers/mali_csf`:

```text
firmware_profile rock5-itx-edk2-v1.1-gpu-shader
```

Measured on the board, same install, same GLTeapot, one reboot apart:

| `mali_csf` profile | log | GLTeapot |
|---|---|---|
| absent | `identity profile disabled` | 29 FPS |
| `…-gpu-shader` | `identity profile enabled (inherited firmware supply)`, then `reset power/idle ready; mapping GPU control pages` | 60 FPS (vsync) |

The 60 matches the 59 FPS of the qualified stage-6 run in
[DISPLAY.md](DISPLAY.md).

## The owner's applications

Amp, Kiri and Turbo Chook are installed as real `.hpkg` packages in
`system/packages`, not as loose binaries. The difference is not cosmetic: a
file under `system/non-packaged/apps` runs, but it is not in
`/boot/system/apps`, Deskbar never lists it, Tracker draws it with the generic
application icon, and it has no way to ship its data files. Amp needs the
last one — `icons::Init()` looks for its Font Awesome face under
`B_SYSTEM_DATA_DIRECTORY/Amp`, and without it every toolbar button was blank
and the app logged `Amp: the Font Awesome icon font is not available`.

`tools/rock5-itx/build-app-packages.sh` builds them on the workstation. Each
application already carries a `tools/package-haiku.sh` for a native Haiku
build; that script targets x86_64 and runs Haiku-only tools, so this one
mirrors it with the cross toolchain and

- rewrites `architecture` to `arm64`;
- reduces `requires` to `haiku >= r1~beta6`. The originals ask for
  `lib:libcurl`, `lib:libsqlite3`, `lib:libtag`, `lib:libscintilla`,
  `lib:liblexilla`, `lib:libpcre2_8`, `cmd:git` and `cmd:curl`; arm64 has no
  repository providing any of them, and an unresolvable package is deactivated
  at boot. The shared libraries are supplied out of band in
  `system/non-packaged/lib`, which is on the default library search path;
- strips debug symbols with the cross `strip`, restores the resources the
  strip removes with `xres`, and then runs **`resattr`** over the same `.rsrc`.
  That last step is easy to miss: `xres` writes resources into the ELF, but
  Tracker and Deskbar read the icon and the signature from file *attributes*.
  On Haiku `mimeset` creates them; cross-building, `resattr -O` copies the
  resources across so `package create` can put them in the package. Without it
  all three appear in the menu with the generic icon.

Each package also carries `data/deskbar/menu/Applications/<App>`, a symlink to
its own binary. That directory — not `/boot/system/apps` — is what the Deskbar
leaf menu lists: `TBarWindow::MenusBeginning()` points the menu at the user's
Deskbar folder if it has one and otherwise at
`B_SYSTEM_DATA_DIRECTORY/deskbar/menu`, which is why only 27 of the 38
applications appear there.

Dropping a package into `/boot/system/packages` on a running system did not
activate it, even with `package_daemon` running; the packages were picked up
on the next boot. Verified on the board: `/boot/system/apps` holds `Amp`,
`Kiri` and `TurboChook`, each with its own `BEOS:ICON` attribute, all three
are in Deskbar's Applications menu with their own icons, all three launch and
log nothing, and Amp's toolbar draws its Font Awesome glyphs.

## Writing it

The NanoKVM cannot stage a multi-GB image (`/data` is 21 GB and nearly full),
so the image goes over the LAN into the SD-card Debian recovery
([RECOVERY.md](RECOVERY.md)):

```sh
python3 tools/rock5-itx/lab.py recover          # boots the recovery OS
python3 -m http.server 8731                     # in the build directory
ssh rock5-debian 'sudo -S -p "" sh -c "curl -sS --fail \
    http://<host>:8731/haiku-rock5full-mmc.image \
    | dd of=/dev/mmcblk0 bs=4M conv=fsync && sync"' < .debian-sd-password
```

The recovery pulls at about 117 MB/s. Progress is visible in field 10 of
`grep -w mmcblk0 /proc/diskstats`. Because Debian under EDK2 has been seen to
corrupt data during heavy I/O, the write is always verified by reading the
image's exact extent back:

```sh
dd if=/dev/mmcblk0 bs=4M count=1545 iflag=fullblock | sha256sum
```

`sudo` on the recovery does not cache credentials between ssh sessions; give it
the password on stdin for every invocation.

Wiping the eMMC is safe. The SoC SPL tries it first, finds no Rockchip FIT
(`Not fit magic`), and falls through to the EDK2 in SPI.

## Making it the boot device

`efibootmgr` on the recovery replaces the HID-driven Setup UI dance. EDK2 v1.1
already publishes `Boot0005* UEFI Samsung 8GTF4R ... eMMC User Data`. The order
used is

```
0000 UiApp, 0004 NanoKVM USB, 0005 eMMC, 0003 SD, 0001 Shell, 0002 MaskROM, ...
```

so the eMMC boots whenever no USB image is attached, while `lab.recover()`
still works by attaching one. The image's ESP uses the removable-media path,
so no new firmware entry has to be created.

## The driver change the boot needed

The first eMMC boot panicked with `did not find any boot partitions!`. The
serial capture showed the loader had read the eMMC correctly and passed the
right partition offset (`0x2400000`), but the kernel attached only EHCI and
`usb_disk` from the FDT bus — `sdhci` never appeared at all.

`src/add-ons/kernel/busses/mmc/sdhci_fdt.cpp` refuses the controller unless a
driver setting admits it:

```c
void* handle = load_driver_settings("sdhci");
if (handle == NULL)
    return false;
const char* profile = get_driver_parameter(handle, "firmware_profile", "", "");
bool admitted = strcmp(profile, RK3588Mmc::kProfile) == 0;
```

Until now nothing supplied that file, because the eMMC was only ever attached
*after* the boot volume was mounted from USB. Booting from it requires the
driver admitted before the first mount, and the refusal is silent.

It works because the boot loader reads every file in
`home/config/settings/kernel/drivers` off the boot volume with its own BFS
reader and hands them to the kernel before anything is mounted
(`src/system/boot/loader/load_driver_settings.cpp`). The image therefore ships
`home/config/settings/kernel/drivers/sdhci`:

```text
firmware_profile rock5-itx-edk2-v1.1-emmc-legacy
read_only false
```

`read_only` defaults to **true** in the same function, and [MMC.md](MMC.md)
documents read-only as the intended mode — the lab deliberately never wrote
this device from Haiku. `mmc_disk` does implement `mmc_block_write`, so this is
a gate rather than a missing feature, but writing the eMMC from Haiku is new
and is not covered by the earlier read-only qualification.

## Why it does not boot yet

With the settings file above the driver does attach. The serial capture of the
eMMC boot shows:

```text
sdhci: noncacheable DMA payload 0x2d80000, 524288 bytes
sdhci: RK3588 eMMC IRQ 237, external clock 375 kHz ... MMC bus object created
mmc_bus: Reset the bus...
mmc_disk: CALLED float mmc_disk_supports_device(device_node*)
mmc_bus: CMD0 result: No error
mmc_disk: Could not get ... scanning the bus
sdhci: Command 8 failed: Operation timed out (status 0x10000)
mmc_bus: Card does not implement CMD8, may be a V1 SD or MMC card
mmc_bus: Trying MMC ...
...
PANIC: did not find any boot partitions!
```

Two things are worth separating. The CMD8 timeout is **normal** — CMD8 is the
SD `SEND_IF_COND` probe and an eMMC does not answer it; `mmc_bus` says so and
falls through to the MMC path. The failure is the ordering around it.

`MMCBus` runs its card scan on a worker thread (`_WorkerThread`, started in the
constructor, driven by `fScanSemaphore`), and the card's node is only
registered at the end of a successful pass:

```c
if (cardFound) {
    ...
    gDeviceManager->register_node(bus->fNode, MMC_BUS_MODULE_NAME, attrs, NULL, NULL);
}
```

Nothing in the boot path waits for that. `vfs_boot.cpp`'s
`get_boot_partitions()` calls `KDiskDeviceManager::InitialDeviceScan()` and
then walks whatever devices exist; its only retry relaxes `strict` matching, it
does not wait for a slow bus. `mmc_disk_supports_device` is offered the bus node
once, while the card is still being initialised, declines ("Could not get …"),
and is never asked again before the panic.

USB boots because `usb_disk` publishes before the scan — the log shows
`publish device: ... disk/usb/0/0/raw` ahead of `get_boot_partitions()`. The
eMMC simply loses that race every time.

### The fix

Making the MMC bus scan synchronously is **not** safe. `register_node()` takes
the device manager's `sLock` and holds it across `Register()`, which is what
calls `register_child_devices`; the recursive lock only helps the *same*
thread, so blocking in `mmc_bus_register_child` while `MMCBus::_WorkerThread`
calls `register_node` from another thread deadlocks by construction.

Instead the boot path now waits for the device rather than giving up on the
first pass, in `vfs_mount_boot_file_system()`
(`src/system/kernel/fs/vfs_boot.cpp`):

```c
const bigtime_t deadline = system_time() + kBootDeviceTimeout;   // 10 s
while (true) {
    status = get_boot_partitions(bootVolume, partitions);
    if (status < B_OK)
        panic("get_boot_partitions failed!");
    if (!partitions.IsEmpty())
        break;
    if (system_time() >= deadline) {
        panic("did not find any boot partitions! @! syslog | tail 15");
        break;
    }
    snooze(kBootDeviceRetryInterval);                            // 250 ms
}
```

`KDiskDeviceManager::CreateDefault()` returns early when the manager already
exists and `InitialDeviceScan()` is a rescan of `/dev/disk`, so repeating the
call is safe. A device that is present on the first pass is still found
immediately, so an unaffected boot behaves exactly as before — only the case
that used to panic now gets up to ten seconds first. This also helps any other
bus that publishes its disks from a worker thread, not just MMC.

## Status

Haiku boots from the eMMC. Observed on 2026-09-20:

- The image is written and verified byte for byte (readback sha256 equals the
  host image).
- `sdhci` attaches during early boot and `mmc_bus` brings the card up:
  `MMC EXT_CSD: revision 8, sectors 15269888, cache enabled 1`.
- The kernel identifies the boot partition by offset and mounts it;
  `/dev/disk/mmc/0/` shows the 33,554,432-byte ESP and the 6,442,450,944-byte
  BFS.
- The desktop comes up with `APPS=38 PREFS=21 DEMOS=10`: the 35 from the
  regular definition plus `Amp`, `Kiri` and `TurboChook` from their own
  packages, with `system/non-packaged/demos` holding `GLTeapot Haiku3d`.
- GLTeapot runs from the install through the fork's OpenGL.
- app_server runs on the `rk3588_display` accelerant, not the framebuffer:
  Screen reports `RK3588 VOP2 HDMI TX1 (Rockchip RK3588 VOP2)` at 1920x1080,
  24 bits, 60 Hz, with the hardware cursor active.
- `mali_csf` reports `identity profile enabled` and takes GPU work from Mesa:
  GLTeapot runs at 60 FPS rather than the 29 of the software rasteriser.
- Amp, Kiri and Turbo Chook are in Deskbar's Applications menu with their own
  icons, launch from `/boot/system/apps` and log nothing; Amp's toolbar draws
  its Font Awesome glyphs from the copy its package installs.
- The volume is writable and persistent: a file written before a reboot is
  still there afterwards, and the second boot goes straight to the desktop
  because the first-login marker was saved.

The first boot shows Haiku's "Welcome" language/keymap prompt, as any
freshly written image does; choosing "Try it out" reaches the desktop and the
choice persists.

Two things are deliberately not claimed. This is not a qualified run in the
project's sense — no two-boot native trial with recorded evidence has been
made for it — and the lab's telnet shell is absent from the *image* because it
needs the private overlay (`~/config/settings/rock5-lab/enable-shell` plus a
shadow file) that the stage pipeline applies at deploy time, not something the
build produces. The read-only eMMC attachment in [MMC.md](MMC.md) remains the
qualified result for that device; writing it from Haiku is new.

The overlay can, however, be applied to a *running* eMMC install rather than
to an image, which is how this install is now driven: write
`system/settings/etc/shadow` (the `baron` line from
`state/haiku-lab-login.json`) and `~/config/settings/rock5-lab/enable-shell`
from a Terminal on the board, then reboot. `UserBootscript` sees both, finds
the RNDIS address and starts `telnetd` on it, so `tools/rock5-itx/shell.py run
10.239.6.100` works exactly as it does for a deployed lab image. The volume is
writable, so this survives reboots; a re-flash removes it again.

After a warm reboot the NanoKVM capture of HDMI1 shows uniformly shifted
colours. That is the known colorimetry/AVI-infoframe finding in
[DISPLAY.md](DISPLAY.md), a property of the link and the capture rather than of
anything rendered.
