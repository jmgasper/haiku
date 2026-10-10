# Mesa's PowerVR Vulkan driver and OpenGL for air/OS (Cubie A7S)

`build.sh` cross-builds the following for Haiku arm64:
- Mesa 26.2.4's Imagination Vulkan driver, `libvulkan_powervr_mesa.so`, with four test programs;
- OpenGL and OpenGL ES on top of that driver through zink (see [OpenGL](#opengl)).
The driver talks to the air/OS kernel driver `powervr` through
`/dev/graphics/powervr/0`. The kernel's userland contract is
`headers/private/graphics/powervr/pvr_haiku.h` in this tree. The build puts
that directory on the include path and does not copy it. The design is
`/mnt/HaikuWork/cubie/evidence/gpu/DESIGN.md` (§3, §5 M3, §7).

    tools/cubie-a7s/mesa/build.sh        # = all
    tools/cubie-a7s/mesa/build.sh shim   # optional host smoke test, below

| Step | What it does |
|---|---|
| `fetch` | Downloads `mesa-26.2.4.tar.xz` from archive.mesa3d.org and checks the pinned sha256 (from Mesa's 26.2.4 release notes; the `.sig` is Eric Engestrom's key). |
| `patch` | Applies `mesa-haiku-pvr.patch`. A changed patch is applied after reverting the old one. |
| `host` | Builds `mesa_clc`, `vtn_bindgen2` and `pco_clc` natively at the same Mesa version. Serialized NIR and USC code must match the driver's version. The build uses the host's LLVM 18, with clang-cpp, LLVMSPIRVLib and SPIRV-Tools from `toolchains/mesa-native-deps` (read only). It does not touch `toolchains/mesa-host`, which holds the 25.3.6 tools. |
| `configure`, `driver` | Cross-builds only `-Dvulkan-drivers=imagination`: no GL, EGL, LLVM, shader cache or WSI. It uses the ROCK 5's pinned sysroot and cross file (`artifacts/mali-system-opengl-build/20260918T125722Z`), as the Pi 4 build does. Debug paths are relative to the root, so every root builds the same bytes. Meson reads machine files only at setup, so when they change the build directory is wiped. |
| `tests` | Builds the test programs against the driver. Linking also checks that every symbol the driver needs exists in the sysroot. |
| `gl` | Builds the EGL vendor library with zink and softpipe in `build-gl/`, the `libvulkan.so.1` shim and `pvr_glprobe`. |
| `all` | Runs everything above and fills `out/`. |

Everything lives under `/mnt/HaikuWork/cubie/mesa` (`CUBIE_MESA_ROOT`):
`downloads/`, `mesa-26.2.4/` (patched), `host-tools/bin`, `build-host/`,
`build/`, `tests/`, and `out/`.

`out/` holds (stripped; `debug/` has the unstripped copies):
- `libvulkan_powervr_mesa.so` and `powervr_mesa_icd.aarch64.json` (for a loader later);
- `pvr_vkprobe`, `pvr_vkfill`, `pvr_vkfence`, `pvr_vktriangle`;
- `libEGL_mesa.so.0`, `10_mesa.json` (its libglvnd vendor file), `libvulkan.so.1` and `pvr_glprobe`;
- `MANIFEST`, with the input hashes.

On the image, the libraries go to `/boot/system/non-packaged/lib`, `10_mesa.json` to `.../non-packaged/add-ons/opengl/egl_vendor.d`, and the programs to `.../non-packaged/bin`.

## The patches

`mesa-haiku-pvr.patch` (the Vulkan driver) and `mesa-haiku-gl.patch` (EGL, hgl, zink) are applied in that order and touch different files. Each header lists its pieces. The Vulkan one, in short:
- `include/xf86drm.h` stands in for libdrm. `drmIoctl()` becomes `ioctl(fd, PVR_HAIKU_OP(request & 0xff), arg, IOCPARM_LEN(request))`, and there are libdrm-style syncobj wrappers.
- `src/util/u_sync_provider_haiku.c` connects those wrappers to the Vulkan runtime, with timelines.
- `pvr_instance.c` opens the device node directly. `HAIKU_PVR_DEVICE` overrides the path.
- `pvr_drm_bo.c` maps buffers with `PVR_HAIKU_NR_MAP_BO` and unmaps them with `delete_area()`.
- Small fixes cover `drm.h`, `pvr_drm.h`, `vk_image` and `pvr_physical_device.c`.

There are no buffer or sync file descriptors yet. Those paths fail with `EOPNOTSUPP`. They forward to the kernel once `pvr_haiku.h` defines `PVR_HAIKU_NR_PRIME_*` or `PVR_HAIKU_NR_SYNCOBJ_{HANDLE_TO_FD,FD_TO_HANDLE}`.

To change a patch:
1. Edit the tree under `$ROOT/mesa-26.2.4`.
2. Diff it against the pristine unpack in `$ROOT/pristine/mesa-26.2.4`: `diff -ruN -x __pycache__ -x '*.pyc' -x '.haiku-*'`, with paths rewritten to `a/`, `b/`, and split by file between the two patches.
3. Keep the headers.
4. Record the result as applied: copy the patches to `$ROOT/mesa-26.2.4/.haiku-patches/1-mesa-haiku-pvr.patch` and `2-mesa-haiku-gl.patch`, and write the sha256 of their concatenation to `.haiku-patches/sha256`. Otherwise `build.sh` reverts the old series and applies the new one over your edits.

What the kernel has to match:
- **Syncobj wait timeouts** are absolute `CLOCK_MONOTONIC` nanoseconds, as on Linux; on Haiku that is `system_time() * 1000`. `INT64_MAX` means wait forever.
- **A wait that runs out of time** must fail. Userland maps `B_TIMED_OUT` and `B_WOULD_BLOCK` to `ETIME`, and does not retry waits on `EAGAIN`.
- **Every other request** is retried on `EINTR`/`EAGAIN`, as libdrm does.

## Tests

The Vulkan tests need `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1`, because 36.56.104.183 is not on Mesa's conformance list. There is no Vulkan loader on arm64 Haiku, so each program links the driver directly and starts from `vk_icdGetInstanceProcAddr`. Put `libvulkan_powervr_mesa.so` in `/boot/system/non-packaged/lib`, or next to the program with `LIBRARY_PATH=%A:$LIBRARY_PATH`.

- `pvr_vkprobe` prints the instance, the properties (name, versions, IDs, UUIDs, DRM nodes), the limits, the 1.0/1.1/1.2 features, the memory heaps and types, the queue families and the device extensions. It then creates a device with one queue, waits for it to go idle and destroys it. `PVR_DEBUG=info` also makes the driver dump the core's details.
- `pvr_vkfill [runs] [timeout-ms]` runs one compute dispatch that writes `gl_GlobalInvocationID.x * 3 + 1` into a 1 MiB storage buffer in HOST_VISIBLE|COHERENT memory. The buffer is prefilled with `0xdeadbeef`. The test waits on a fence, checks every word, and prints the first 16 mismatches, the count and the timings.
    - The shader is SPIR-V 1.0 in the source as words. There is no glslang anywhere in the lab, so it was written in SPIR-V assembly (quoted in the source), assembled with `spirv-as` and validated with `spirv-val --target-env vulkan1.0`, both from `toolchains/mesa-native-deps/usr/bin`.
- `pvr_vkfence [timeout-ms]` submits with no command buffers, which takes the null-job path. That submit signals a fence and timeline value 1. The test then:
    - waits on both, and checks the counter;
    - submits again, waiting on 1 and signalling 2;
    - signals 3 from the host;
    - checks that a wait for 10 and a wait on a reset fence each return `VK_TIMEOUT` after 10 ms.

- `pvr_vktriangle [--linear] [--ppm FILE] [--timeout MS]` draws one triangle into a 64x64 `R8G8B8A8_UNORM` image cleared to (51, 102, 153, 255), copies it to a host-visible buffer, and compares every byte with a CPU reference.
    - The fragment colour is a constant (204, 153, 51, 102), and the corners are framebuffer points (0, 0), (64.25, 0) and (0, 64.25), taken from `gl_VertexIndex`. A pixel is covered if and only if x + y <= 63, and no pixel centre lies on an edge.
    - On the GPU this is a render submit (GEOMETRY, partial-render FRAGMENT and FRAGMENT on an HWRT data set with its own free list), then one TRANSFER_FRAG job for the copy.
    - `--linear` renders into a LINEAR image and reads it in place, which needs no transfer job.
    - It prints the first mismatches, counts of triangle, clear and unwritten pixels, and a map of every fourth row and column when anything is wrong.

Every program prints `PASS`/`FAIL` and exits 0/1. Output is line-buffered, so it survives a crash.

## OpenGL

`libEGL_mesa.so.0` is Mesa's EGL vendor library for libglvnd. It also carries GL and GLES, so libglvnd's `libEGL.so.1`, `libGLESv2.so.2` and the Pi's BGLView `libGL.so.1` reach it through `10_mesa.json`. It replaces the Pi's Mesa 25.3.6 (v3d + softpipe) on the Cubie.

`eglInitialize` chooses the screen:
- **zink**, on the PowerVR Vulkan driver, when `/dev/graphics/powervr/0` (or `HAIKU_PVR_DEVICE`) exists and neither `LIBGL_ALWAYS_SOFTWARE` nor a `GALLIUM_DRIVER` other than `zink` is set;
- **softpipe** otherwise, or when zink fails: no `libvulkan.so.1`, no Vulkan device, or a failed device creation. One line on stderr says so.

Zink needs the core to be visible, so the EGL driver sets `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1` while it creates the zink screen, unless the process has set it already, and removes it again afterwards. There is no llvmpipe, because the sysroot has no LLVM for arm64 Haiku.

Window surfaces (BGLView, GLTeapot) on zink:
1. Each swap reads the frame back: zink copies the GPU image to host memory and waits.
2. The frame goes into one of two `B_RGBA32` bitmaps.
3. The window's `BitmapHook` shows that bitmap. A bitmap is written again only after `SetBitmap()` has handed it back.

softpipe keeps drawing into software display targets, as before.

Zink exposes OpenGL 2.1 (GLSL 1.20) and OpenGL ES 2.0 (GLSL ES 1.00) on this core. These figures come from the host shim run below. The PowerVR driver has no transform feedback, geometry shaders or conditional rendering.

Zink keeps its shader cache in the usual Mesa cache directory, limited to 128 MB (`MESA_SHADER_CACHE_DISABLE=true` turns it off). Its key includes the library file's timestamp.

**Why `libvulkan.so.1` is a shim.** There is no Vulkan loader on arm64 Haiku. Zink `dlopen()`s `libvulkan.so.1` and takes `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` from it. `vulkan_shim.c` provides those two, plus the global commands, by forwarding to the driver's `vk_icdGetInstanceProcAddr`. It links the driver by name. This keeps zink unpatched and gives any program the usual way in. It is not a loader: no layers, one driver, no window system surfaces. A ported Khronos loader would replace it.

`pvr_glprobe [--expect TEXT] [--ppm FILE]` checks GL on the board in one command:
- It prints the EGL vendor and version, then `GL_RENDERER`, `GL_VERSION` and `GL_SHADING_LANGUAGE_VERSION`.
- It clears a 64x64 pbuffer, then draws the same triangle reference as `pvr_vktriangle` (constant colour, no pixel centre on an edge), and checks every byte read back. A second frame repeats the check.
- `--expect zink` fails the run unless the renderer is zink, as in `zink Vulkan 1.2(PowerVR B-Series BXM-4-64 MC1 (IMAGINATION_OPEN_SOURCE_MESA))`.
- `LIBGL_ALWAYS_SOFTWARE=1 pvr_glprobe --expect softpipe` checks the fallback.

## Host smoke test (`build.sh shim`)

`build.sh shim` builds the same patched tree for Linux; the Haiku hunks compile out. It adds Mesa's pvr drm-shim (`PVR_SHIM_DEVICE_BVNC=36.56.104.183`) and `pvr_ioctl_trace.c`, an `LD_PRELOAD` tracer. It then runs the Vulkan tests (and `pvr_vktriangle --linear`) and `pvr_glprobe` on the build host.

All of the driver's own code runs: enumeration, device creation, the PowerVR compiler on this core, command streams and null jobs. Every DRM ioctl lands in `shim/<test>.log`, in order and with its flags, so the logs show what the kernel will receive. The differences on Haiku are that `GET_BO_MMAP_OFFSET` + `mmap()` become `MAP_BO`, and enumeration opens the node by path.

Every test runs with 256 KiB stacks (`ulimit -s 256`), Haiku's default for an application's thread. A stack frame too large for the board crashes here too; this is how the `pvr_queue_transfer()` overflow reproduces off-board.

The shim executes nothing. So in the expected results:
- `pvr_vkprobe` passes;
- `pvr_vkfill` reports every word unwritten;
- `pvr_vkfence` reports the 9 checks that need real syncobj state (values, `NOT_READY`, timeouts).
- `pvr_vktriangle` (both modes) reports every pixel never written.
- `pvr_glprobe --expect zink` runs zink on the PowerVR driver through Linux EGL (surfaceless, the shim's render node, `MESA_LOADER_DRIVER_OVERRIDE=zink`). It prints the zink renderer and OpenGL ES 2.0, links the program, and submits render and transfer jobs. The pixels it reads back are zeros.
