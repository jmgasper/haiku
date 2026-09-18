# X399 workstation status

Status values are backed by observation on the workstation. Anything not
listed as verified is untested.

| Area | Goal | Status |
| --- | --- | --- |
| Boot from NVMe (UEFI) | Haiku boots directly from the Samsung 950 PRO | verified: firmware boots `EFI/BOOT/BOOTX64.EFI` from the NVMe ESP; `/boot` is `/dev/disk/nvme/0/1` |
| SMP | all 32 hardware threads (16 cores) online | verified: `sysinfo` lists 32 CPUs |
| NVMe | disk available and bootable | verified after multi-root PCI fix: 2 GiB raw read at 1.6 GiB/s, boot volume |
| Ethernet | I211 up with DHCP | verified: ipro1000 link 1000BASE-T, DHCP lease, HTTP upload and SSH |
| USB | all controllers and ports enumerate devices | all 5 xHCI controllers start after the PCI fix; NanoKVM device enumerates on the ASM2142; other ports need physical devices |
| Audio | ALC1220 analog output, HDMI audio | AMD HDA and GP102 HDMI controllers attach (`/dev/audio/hmulti/hda/0,1`); playback untested |
| Graphics | GTX 1070/1080 Ti accelerated 2D/3D, 3-4 monitors | in progress: two displays (HDMI and DisplayPort) drive one 3840x1080 desktop, Vulkan runs shaders on the GPU (1.4 TFLOP/s compute, 55 Gpixel/s fill), and Haiku's OpenGL kit renders through the GPU and presents into app_server's bitmap without the CPU: GLTeapot runs at 2390 fps against 238 in software, a lit sphere at 1600x900 at 230 fps using a third less processor time than presenting through the CPU |
| Sleep | S3 suspend and resume | sleeps and wakes; the display comes back, but the NVMe and the network card usually do not |

## Log

- 2026-09-17: hardware inventory captured with SystemRescue. The NVMe
  contained an ARM64 Haiku test install from the ROCK 5 lab; its files,
  EFI partition and GPT were backed up before reuse.
- 2026-09-17: stock nightly (hrev60097) booted from NanoKVM virtual USB.
  The second Threadripper host bridge (root bus 0x40: NVMe, GPU, one CPU
  xHCI, one AHCI) was not enumerated. After reading every ACPI host bridge,
  its resource windows and `_PRT`, the NVMe, the fifth xHCI controller and
  the GPU's HDMI audio function appear.
- 2026-09-17: installed the system to the NVMe (GPT: FAT ESP + BFS "X399")
  from the live image and booted it without removable media.
- 2026-09-17: `nvidia_rm` (X547's OS layer + NVIDIA 570.86.16 proprietary RM
  core) completes `RmInitAdapter` on the GTX 1080 Ti; app_server uses the
  NVKMS accelerant at 1920x1080.
- 2026-09-17: NVK (X547's RM backend + pre-Volta patch, built natively on the
  workstation with LLVM 20, libclc and SPIRV-LLVM-Translator) enumerates
  "NVIDIA GeForce GTX 1080 Ti (NVK GP102-A)" with Vulkan 1.3 and creates a
  device. The GPU fetches the GPFIFO and loads the channel's context (with the
  RM watchdog disabled), but submitted work does not complete yet. The RM runs
  its own watchdog channel without errors. `nvidia_rm` gained an RC timer,
  registry settings (`~/config/settings/kernel/drivers/nvidia_rm`,
  `RegistryDwords "Key=Value;..."`) and development register/VRAM readers.
- 2026-09-17: the accelerant drives every connected display on its own head
  and presents one spanning desktop (left to right, each display at its
  preferred mode). Verified with the NanoKVM on HDMI-0 and DP-0 forced on
  through `force_connected DP-0` in `~/config/settings/nvidia_rm_accelerant`.
- 2026-09-17: fixed an early-boot panic with on-screen debug output: 32 CPUs
  printing during application processor start-up made `dprintf` trip the
  spinlock deadlock detector.
- 2026-09-17: ACPI S3 on x86_64. A real mode trampoline at 0x81000 resumes
  the boot CPU, which restarts the other CPUs through INIT/SIPI and keeps the
  TSC continuous. The PCI root driver restores configuration space and MSI-X
  tables, `nvme_disk` resets its controller, and `nvidia_rm` suspends NVKMS and
  the RM; the accelerant sets the mode again after resume. Verified by waking
  with the power button: the syslog of the resumed session reaches the disk and
  the display comes back, but its contents are not redrawn yet. Testing uses
  `x86suspend s3 [flags]` (generic syscall `x86_suspend`). Later on the same
  day the workstation stopped reacting to the NanoKVM power button and needs a
  power-cycle at the wall.
- 2026-09-18: suspending with 32 CPUs deadlocked, because parking the
  application processors one at a time while the scheduler was still running
  let a halted processor block anything that waits for it. They are parked at
  once now, with interrupts already disabled, and the file systems are synced
  before the system is quiesced. Message signaled interrupts stopped working
  after resuming until the HyperTransport MSI mapping was restored along with
  configuration space; PME is cleared while sleeping, since a network card
  left in wake on LAN mode woke the machine seconds after it slept.
  Suspending can be traced step by step, the trace is also kept in memory
  (`x86suspend trace`, or `x86suspend watch <host> <port>` to have it reported
  over the network), and single parts can be skipped for bisecting.
  Sleeping and waking themselves work: all 32 CPUs come back, and the
  accelerant restores the display. The NVMe and the network card, however,
  usually stay dead afterwards, which also blocks anything that needs the
  disk (a clean shutdown, for instance). One run had everything back,
  including the network 8 seconds after waking, so the failure is not
  systematic. Skipping the USB, NVMe or driver power hooks does not change
  it, all CPUs demonstrably restart, and the IOMMU is disabled on this board,
  so the cause is still open. Debugging it further needs a channel that
  survives the failure: the machine cannot write its log to disk, and the
  HDMI capture of the NanoKVM returns stale frames.
- 2026-09-18: Vulkan works. Submitted work never completed because a mapping
  smaller than a page came back pointing at the start of its page: RM rounds
  the ranges of a mapping context down to a page boundary and hands the offset
  inside the page to the caller in `pLinearAddress`, which neither the Haiku
  RM clients nor the driver added back. A channel's USERD is 512 bytes and
  eight of them share one page, so every channel after the first wrote its
  GPPut into the first channel's USERD: the wrong channel was kicked and the
  submitting one never ran. It showed up as GPFIFO entries being consumed with
  nothing executing, no MMU fault and no semaphore release; submitting an entry
  pointing at unmapped memory, which faulted on one channel but was silently
  swallowed on the other, is what pinned it down. The driver now also releases
  the mapping context after use, as `nvidia_mmap()` does on Linux, instead of
  leaving it to be reused by the next mapping on the same file descriptor.
  With that, `vkprobe` passes reliably: a compute-free buffer copy and a
  rendered triangle read back correctly, waiting on a real GF100 semaphore
  release rather than polling.
- 2026-09-18: DisplayPort detection investigated with the new `nvdpyinfo`
  tool. NVKMS only learns about a DisplayPort connection through a hotplug
  event from RM, and RM itself reports no connection on any of the three
  physical DisplayPort connectors: the hot plug detect line reads low (a
  detection with DDC and load detection disabled returns only HDMI-0), and a
  DPCD read over the AUX channel of DP-0, DP-2 and DP-4 gets no reply, while
  the same query on the HDMI connector reports it connected. So the GPU sees
  nothing on those connectors at all, which points at the cable, the port or
  the monitor rather than at the driver. `nvdpyinfo --watch <seconds>` polls
  both RM and NVKMS and reports every change, to catch a plug event when one
  happens.
- 2026-09-18: a GTX 1070 (GP104, 0x10de:0x1b81) works with no driver change -
  the driver binds to any NVIDIA display device rather than a list of
  identifiers - and its DisplayPort output is detected: a Dell U2414H on DP-4
  comes up beside the HDMI display, both driven by their own head, for one
  3840x1080 desktop. So the DisplayPort connectors of the 1080 Ti that never
  reported a connection were a hardware matter, not a driver one.
- 2026-09-18: `vkbench` (tests/vkbench.c, built and run by
  tools/build-vkbench.sh) measures compute throughput, fill rate and copy
  bandwidth with real SPIR-V shaders. Its first numbers exposed that every
  submission took exactly one second: NVK waited for the channel's semaphore
  by polling an RM event with a one second timeout, and resman never delivers
  that event on this GPU, although GPU interrupts themselves do arrive.
  Spinning on the semaphore first, with a one millisecond poll behind it,
  turned 17 GFLOP/s into 1.4 TFLOP/s and 0.41 into 55 Gpixel/s. On the
  GTX 1070: 1.4 TFLOP/s of fused multiply-adds, 55 Gpixel/s fill,
  58 GB/s copy within video memory and 6.1 GB/s upload over PCIe.
  Delivering the completion interrupt to the waiting thread instead of
  polling is still open.
- 2026-09-18: OpenGL reaches the GPU. Haiku's OpenGL kit (libGL.so and the
  renderer add-ons in add-ons/opengl) belongs to the mesa package, 22.0.5 here,
  so the Zink renderer is built from that same release - the add-on and libGL
  then share one glapi - while Zink itself talks to NVK through the Vulkan
  loader, which is a stable interface between the two Mesa versions. The new
  add-on (src/gallium/targets/haiku-zink in src/mesa-build/mesa-hgl, built by
  tools/build-mesa-hgl.sh) is the software renderer's target with the screen
  created by `zink_create_screen`, and HGL_NO_ZINK falls back to software.
  `gltest` (tests/gltest.cpp) reports
  `renderer: zink (NVIDIA GeForce GTX 1070 (NVK GP104-A))`, OpenGL 4.5, and
  clears and presents 800x600 frames at 186 fps, against 43 fps for the same
  window drawn by softpipe.
  Three things had to be fixed on the way: the page kind table in NVK's resman
  backend was Turing's, so every depth buffer was refused on Pascal; the front
  buffer has to be flushed with the pipe context, which a hardware driver needs
  to read the image back; and the Haiku winsys asked BBitmap for a default row
  size while advertising an aligned one, so Zink's copy ran off the end of the
  bitmap.
  What is left: a draw call stops the graphics engine with a class error
  (Xid 69), after a few frames have already been presented, while the same
  draws issued directly through Vulkan by `vkbench` (including depth buffers
  and vertex buffers) run fine. NVK's flush now gives up after
  NVK_NVRM_TIMEOUT_MS instead of hanging, which makes NVK dump the command
  buffer that failed; it ends with the ordinary draw macro, so the offending
  state is somewhere earlier in the stream.
- 2026-09-18: the draw that stopped the graphics engine was an unaligned
  constant buffer. Diffing the command stream NVK builds for a Zink draw
  against one for the same draw issued straight through Vulkan left exactly
  three methods: SET_CONSTANT_BUFFER_SELECTOR_A/B/C. Its address,
  0x205700140, is 64 byte aligned, and a pre-Turing GPU wants 256 there;
  binding it that way raises a class error (Xid 69) and the channel is dead
  from then on. The address is a dynamic uniform buffer, whose offset comes
  from the caller, and the caller is Mesa's OpenGL state tracker: it uploads
  the default uniform block at a hard coded alignment of 64. On Turing, where
  NVK asks for 64, that happens to be right. It now uses the alignment the
  driver reports, and OpenGL draws correctly.
  What is left in this area: presenting a frame still copies the image back
  through the CPU into a BBitmap, which is what limits a small window to a few
  hundred frames a second.
- 2026-09-18: the GPU now writes the frame into app_server's own bitmap.
  Presenting used to read the image back with the CPU and copy it into the
  window's bitmap; the bitmap's pages are now pinned and mapped into the GPU's
  address space, so the copy that presents a frame is a GPU write into the
  memory app_server already reads, and the CPU never touches the pixels. That
  needed three pieces: `os_lock_user_pages()` in the driver, which was a stub
  that panicked and now locks the pages with lock_memory_etc(); support for
  VK_EXT_external_memory_host in NVK's resman backend, built on resman's OS
  descriptors; and a present path in Zink that imports the window system's
  buffers once and copies into them on the GPU. Redrawing a window no longer
  touches the GPU at all - the frame is already in the bitmap.
  Measuring it also exposed that every submission was costing a millisecond:
  the flush waited with poll(), whose shortest sleep is a whole tick, however
  short a timeout is asked for. Spinning first and then sleeping in short steps
  took an empty submission from 1.08 ms to 13 us, which is worth far more than
  the present path itself: the lit sphere at 800x600 went from 167 to 500 fps,
  and GLTeapot from 363 to over 1000.
  The frame rate depends heavily on the GPU's clocks, which only ramp under
  sustained load, so measurements are only comparable after a warm up.
  What is left in this area: presenting still crosses the PCIe bus, and the GPU
  writes system memory at only 1.6 GB/s (it reads at 6.2). Presenting into the
  screen's own frame buffer in video memory, through Haiku's direct window
  mode, would avoid the bus entirely.
- 2026-09-18: measured, with the GPU warmed up first, since its clocks only
  ramp under sustained load. A lit sphere at 1600x900: 230 fps and 1.0 s of
  processor time per 5 s with the GPU presenting, against 186 fps and 1.6 s
  when the frame is copied by the CPU - a fifth more frames for two thirds of
  the processor time, and the gap grows with the window, since the copy the CPU
  no longer makes is proportional to its area. At 800x600 the frame rates are
  the same and only the processor time differs (2.0 s against 2.6 s).
  GLTeapot reaches 2390 fps, against 238 on the software renderer.
- 2026-09-18: the machine froze twice - no network, no keyboard, a still
  desktop whose clock had stopped - and the second freeze left the file system
  damaged. The cause was in the page pinning added for presenting:
  os_lock_user_pages() recorded B_CURRENT_TEAM, which is not a team but a
  sentinel meaning whichever team is running. The pages are unlocked when
  resman tears the memory down, which happens while the application is being
  cleaned up and can run in another team's context, so the unlock went to the
  wrong address space. Recording the team that locked the range fixes it:
  tools/gl-stress.sh now runs the test repeatedly, killing it while the GPU is
  writing into its window, and the machine stays up with every pinned range
  released (64 locked, 64 released).
  The kernel debugger could not be reached over the KVM's USB keyboard, so the
  diagnosis came from the code rather than from the frozen machine; a serial
  console would have made it quicker.
- 2026-09-18: the GPU had been talking to the machine at PCIe 2.5 GT/s, a
  quarter of what the link can carry. Both ends advertise 8 GT/s, but the link
  drops to its slowest speed whenever the GPU is idle and nothing here raised
  it again - resman does that from the power management that this driver does
  not run. NVK now asks resman to train the link when it opens the device.
  Copies into memory the application owns went from 1.6 GB/s to 4.6, and
  reading a frame back from 1.6 to 2.8; `nvdpyinfo --rm` reports the link, and
  `--pcie-speed <gen>` retrains it by hand.
- 2026-09-18: a program other than the accelerant can now draw straight into
  the screen. The accelerant's frame buffer is an ordinary resman memory object
  in video memory, so after each mode set it shares that object
  (`NV0000_CTRL_CMD_CLIENT_SHARE_OBJECT`, giving away the right to duplicate it)
  and tells the driver where it is; anyone can then ask the driver for it
  (`NV_HAIKU_GET_SCANOUT`) and duplicate the handle into its own client
  (`NV_ESC_RM_DUP_OBJECT`). `nvscanout` proves the mechanism from a plain
  program: it duplicates the handle, maps it and paints a band that appears on
  the monitor.
  NVK imports the same memory through a private agreement on the pNext chain of
  `vkAllocateMemory` (`VkImportScanoutMemoryHAIKU`, in `vk_haiku_scanout.h`),
  which needs nothing of the Vulkan loader. `vkbench scanout` then has the GPU
  copy a frame into it at 7.8 GB/s - a whole 1920x1080 frame in 1.07 ms,
  against 2.93 ms to send the same frame to system memory. The screen turned
  the colour the GPU filled it with, which is the proof and also the warning:
  writing the whole frame buffer tramples what app_server drew, so the renderer
  must copy only the window's visible rectangles.
  What is left in this area: Zink has to use it - present into the scanout at
  the window's position, clipped to what Haiku's direct window mode reports is
  visible, and fall back to the existing host-memory path when the window is
  not direct-connected.
- 2026-09-18: OpenGL now presents straight into the screen. A window in Haiku's
  direct mode is told where it sits and which rectangles of it are visible, so
  Zink copies the frame into those rectangles with the GPU rather than into the
  window system's bitmap, which app_server would then have to write to the
  screen a second time. A lit sphere at 1600x900 went from 210 to 969 frames
  per second - four and a half times - and a calculator opened over the window
  stays untouched while the sphere keeps turning around it, so the clipping is
  honoured. `ZINK_NO_SCANOUT_PRESENT` and `GLTEST_NO_DIRECT` both fall back.
  The accelerant had to advertise `B_PARALLEL_ACCESS` in its modes: without it
  `BDirectWindow::SupportsWindowMode()` says no and no window is ever direct
  connected. The frame buffer really can be written while the GPU draws, so the
  flag is honest.
  tools/gl-stress.sh still passes - the test killed three times while the GPU
  was writing the screen, the machine up and every pinned range released.
  What is left in this area: a window that stops drawing is not repainted when
  something uncovers it, because the window system's bitmap has never had a
  frame in it; the renderer should keep the last frame and put it back on a
  redraw. Presenting is also not synchronised with the display, so a frame can
  tear.
