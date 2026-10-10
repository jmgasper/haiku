# Mesa's PowerVR Vulkan driver and OpenGL for air/OS (Cubie A7S)

`build.sh` cross-builds the following for Haiku arm64:
- Mesa 26.2.4's Imagination Vulkan driver, `libvulkan_powervr_mesa.so`, with five test programs and a benchmark;
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
| `gl` | Builds the EGL vendor library with zink and softpipe in `build-gl/`, the `libvulkan.so.1` shim, `pvr_glprobe`, `pvr_glbench` and `pvr_glreset`. |
| `all` | Runs everything above and fills `out/`. |

Everything lives under `/mnt/HaikuWork/cubie/mesa` (`CUBIE_MESA_ROOT`):
`downloads/`, `mesa-26.2.4/` (patched), `host-tools/bin`, `build-host/`,
`build/`, `tests/`, and `out/`.

`out/` holds (stripped; `debug/` has the unstripped copies):
- `libvulkan_powervr_mesa.so` and `powervr_mesa_icd.aarch64.json` (for a loader later);
- `pvr_vkprobe`, `pvr_vkfill`, `pvr_vkfence`, `pvr_vktriangle`, `pvr_vkhang`, `pvr_vkbench`;
- `tls_generation_check` and `libtls_generation_check.so` (keep them in the same directory);
- `libEGL_mesa.so.0`, `10_mesa.json` (its libglvnd vendor file), `libvulkan.so.1`, `pvr_glprobe`, `pvr_glbench` and `pvr_glreset`;
- `MANIFEST`, with the input hashes.

On the image, the libraries go to `/boot/system/non-packaged/lib`, `10_mesa.json` to `.../non-packaged/add-ons/opengl/egl_vendor.d`, and the programs to `.../non-packaged/bin`.

## The patches

`mesa-haiku-pvr.patch` (the Vulkan driver) and `mesa-haiku-gl.patch` (EGL, hgl, zink) are applied in that order and touch different files. Each header lists its pieces. The Vulkan one, in short:
- `include/xf86drm.h` stands in for libdrm. `drmIoctl()` becomes `ioctl(fd, PVR_HAIKU_OP(request & 0xff), arg, IOCPARM_LEN(request))`, and there are libdrm-style syncobj wrappers.
- `src/util/u_sync_provider_haiku.c` connects those wrappers to the Vulkan runtime, with timelines.
- `pvr_instance.c` opens the device node directly. `HAIKU_PVR_DEVICE` overrides the path.
- `pvr_drm_bo.c` maps buffers with `PVR_HAIKU_NR_MAP_BO` and unmaps them with `delete_area()`.
- There is a second memory type: DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED, not HOST_COHERENT (`pvr_host_cache.h`).
    - Its buffers are mapped write-back cached with `PVR_HAIKU_MAP_BO_CACHED`. The coherent type's maps are write-combined, which is slow to read.
    - `vkFlushMappedMemoryRanges()` cleans the range (`dc cvac`) and `vkInvalidateMappedMemoryRanges()` cleans and invalidates it (`dc civac`). Before this they did nothing. `nonCoherentAtomSize` is the larger of 64 and the cache line size from `CTR_EL0`.
    - The coherent type stays type 0, so applications that ask for HOST_VISIBLE or HOST_COHERENT still get it first. Neither type's flags are a subset of the other's, so the Vulkan ordering rule allows either order.
    - `PVR_CACHED_MEMORY_TYPE=0` hides the type, and `=1` shows it off Haiku.
    - Zink needs `mesa-haiku-gl.patch` to use it: see [OpenGL](#opengl).
- A GPU reset loses the device. When the kernel refuses a job with `EIO` (or `ENODEV`), the winsys returns `VK_ERROR_DEVICE_LOST` instead of `VK_ERROR_OUT_OF_DEVICE_MEMORY`. This covers render, compute, transfer and null jobs. `pvr_arch_queue.c` then marks the queue lost and signals that submission's semaphores and fence, which wakes any thread already waiting on them. Every later wait, submit or status query returns `VK_ERROR_DEVICE_LOST` at once.
- Dynamic rendering reuses render target data sets. Each `vkCmdBeginRendering()`, which zink uses for every render pass, gets a render state of its own. That used to mean a new HWRT data set and local free list for every frame, destroyed when the command buffer was reset. In the kernel, each create and destroy meant firmware objects, buffers, VM maps and synchronous firmware cleanups.
    - When a render state is cleaned up, its data sets now go to a device cache (`pvr_rt_dataset.c`). The next rendering with the same width, height, samples and layers takes one.
    - A data set still belongs to one render state at a time. It only enters the cache once its command buffer is no longer pending.
    - The cache keeps at most 16 idle data sets. One is destroyed after 256 later releases, so sizes no longer drawn age out. A data set left in the middle of a render (`need_frag`) is destroyed instead of cached.
    - Render pass framebuffers keep their own data sets, as before.
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
- **`SUBMIT_JOBS` on a context ended by a GPU reset** must fail with `EIO`, which is Haiku's `B_IO_ERROR` (the `-EIO` of Linux code, translated). `ENODEV` works as well. Any other error reads as out of memory: zink retries it for about 1.5 s before it gives up.
- **`MAP_BO` takes the 32-byte `struct pvr_haiku_map_bo`, with `flags`.** A `static_assert` checks the size. With `PVR_HAIKU_MAP_BO_CACHED` the clone must be write-back cached. Userland keeps it coherent itself, which needs EL0 cache maintenance (`SCTLR_EL1.UCI`) and an EL0-readable `CTR_EL0` (`UCT`). A buffer must be zeroed and cleaned when it is created.
- **After a reset, `SYNCOBJ_SIGNAL` and `SYNCOBJ_TIMELINE_SIGNAL` must still work** on that open file and wake waiters, including `WAIT_FOR_SUBMIT` waiters on a point that has no fence. The loss handling depends on them.

## Tests

The Vulkan tests need `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1`, because 36.56.104.183 is not on Mesa's conformance list. There is no Vulkan loader on arm64 Haiku, so each program links the driver directly and starts from `vk_icdGetInstanceProcAddr`. Put `libvulkan_powervr_mesa.so` in `/boot/system/non-packaged/lib`, or next to the program with `LIBRARY_PATH=%A:$LIBRARY_PATH`.

- `pvr_vkprobe` prints the instance, the properties (name, versions, IDs, UUIDs, DRM nodes), the limits, the 1.0/1.1/1.2 features, the memory heaps and types, the queue families and the device extensions. It then creates a device with one queue, waits for it to go idle and destroys it. `PVR_DEBUG=info` also makes the driver dump the core's details.
- `pvr_vkfill [runs] [timeout-ms] [--cached]` runs one compute dispatch that writes `gl_GlobalInvocationID.x * 3 + 1` into a 1 MiB storage buffer in HOST_VISIBLE|COHERENT memory. The buffer is prefilled with `0xdeadbeef`. The test waits on a fence, checks every word, and prints the first 16 mismatches, the count and the timings.
    - `--cached` uses the HOST_CACHED type instead. It flushes the fill before the dispatch and invalidates the buffer before the check. This tests the kernel's cached clone and the cache maintenance.
    - The "checked in" time includes the invalidate, so the coherent and cached runs compare reading 1 MiB from write-combined memory with reading it from cached memory.
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

- `pvr_vkhang [--timeout MS] [--no-recovery] [--hang-ms N]` submits a compute shader that spins forever on a storage buffer the host leaves at 0, so the kernel's job timeout and GPU reset must end it. It waits on a fence (default 180 s) and reports the result: `VK_ERROR_DEVICE_LOST` or `VK_SUCCESS` means the kernel/firmware recovered; `VK_TIMEOUT` means it did not, a FAIL. Then it reads the shader's progress counter, dispatches `pvr_vkfill`'s shader again on the same device, and once more on a freshly created device, printing the `VkResult` of each so you can see whether the reset recovered the context, or the device is lost. `--no-recovery` stops after the counter. `--hang-ms N` instead runs a bounded loop of about N ms (iteration count calibrated against the GPU, so approximate) and checks that a long but legal job is not killed. Each step is timestamped and the output is line-buffered: the process can be lost with the device.

Every program prints `PASS`/`FAIL` and exits 0/1. Output is line-buffered, so it survives a crash.

### Benchmarks: where a frame's time goes

`pvr_glbench` and `pvr_vkbench` run a steady load for a long time and print one line per interval. A slowdown over time shows up in the line where it starts, and the bench it shows up in tells you which layer causes it.

`pvr_glbench [--seconds N] [--size WxH] [--readback WxH] [--interval S] [--frames N] [--resize N] [--expect TEXT] [--mark]` is GLTeapot's frame on zink without a window. It uses the same EGL setup as `pvr_glprobe`: a pbuffer (default 300x300) with depth and an ES 2 context. Each frame:
1. clears colour and depth;
2. draws a lit, rotating torus of 3072 triangles with one `glDrawElements()`, then calls `glFlush()`;
3. reads back the centred region of up to 300x300 with `glReadPixels()`, as each BGLView swap on zink does.

`--readback 0` replaces the readback with `glFinish()`, which separates the readback's own cost from the GPU wait. `--resize N` switches every N frames to the next of three pbuffers: `--size`, 3/4 of it and 1/2 of it. That gives render targets of new sizes, then the same sizes again. Frame 0 compiles the pipeline and gets a line of its own. After that, each line (default every 10 s, `--seconds` default 300) shows:
- frames/s in that window;
- the average ms per frame, split into draw+flush and readback. Draw+flush is only the application thread's share: zink's threaded context records and submits the frame on a thread of its own. The readback waits for that thread and for the GPU, so their time lands there;
- `areas`: the process's areas. `powervr` counts its `powervr buffer` areas, the `MAP_BO` clones;
- `kernel`: the kernel's `powervr ...` areas. Listing the kernel team needs root, as `listarea` does; otherwise it shows `n/a`;
- `covered`: the share of the last frame's pixels the torus drew.

At the end, the run prints the first and last window's frame rates, and the area names whose count changed since frame 0, for the process and for the kernel. It fails if GL reported an error or no frame drew a pixel.

`pvr_vkbench [--seconds N] [--interval S] [--dispatches N] [--timeline] [--rerecord] [--mark]` is the same measurement without zink, GL, the readback and app_server. It loops `pvr_vkfill`'s dispatch, each followed by a fence wait. Each line shows:
- dispatches/s;
- ms per dispatch in `vkQueueSubmit()` and in the wait;
- the same area counts as `pvr_glbench`;
- the result of one extra, checked dispatch.

`--timeline` waits the way zink does: a timeline semaphore signalled by each submit, then `vkWaitSemaphores()`. `--rerecord` records the command buffer again for every dispatch, as zink does for every batch.

How to read the two:
- If GL frames slow down over time and `pvr_vkbench --timeline --rerecord` stays flat, the cause is above the Vulkan driver.
- If both slow down, it is in the driver or the kernel.
- If the `kernel` count keeps growing, the end-of-run list names what grows.

`--frames N` / `--dispatches N` with `--mark` makes a short, marked run for a trace. `build.sh shim` uses it below.

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

**After a GPU reset** (a hung job), zink's next submit gets `VK_ERROR_DEVICE_LOST`. It logs `ZINK: vkQueueSubmit failed (VK_ERROR_DEVICE_LOST)`, submits nothing more, and stops waiting. Before this fix it waited forever. Rendering then does nothing, and `glGetError()` stays `GL_NO_ERROR`. Only a context created with a lose-context-on-reset strategy hears of the reset, through `glGetGraphicsResetStatus`. To draw again, the program has to make a new context; the device is gone for the whole process. `mesa-haiku-gl.patch` also stops `zink_wait_on_batch()` from asserting when a flush on a lost device yields no batch.

A lost device must also let the program quit. GLTeapot used to hang after a reset, and `hey GLTeapot quit` never returned.
- Zink never submits a batch ended after the loss, so the batch stayed "unflushed" for good. Any later wait on something that batch used blocked in `zink_batch_usage_unflushed_wait()`.
- A BGLView present maps the frame the previous batch drew, so the render thread blocked there inside `SwapBuffers()`. It held the GL lock and GLTeapot's quitting semaphore, so the window thread's `DetachedFromWindow()` waited for it as well.
- Batch states were never recycled either, so a program that kept running after the loss grew by a batch state, with all it held, every flush.

`mesa-haiku-gl.patch` (`zink_batch.c`, `zink_context.c`) now does the following:
- It retires such a batch: the batch gets a batch id and is marked flushed, submitted and completed.
- It resets the batch states handed back to the screen, which the submit thread used to do after each submit.
- It treats every batch state as reusable once the device is lost.
- It starts a new batch after a failed submit, instead of recording into the ended one, which the next flush would have put on the list a second time.
- The flush wakeup can no longer be lost.

After a reset, rendering stops but the program keeps running, and it quits normally.

`pvr_glreset [--frames N] [--seconds S]` checks this with a desktop OpenGL context, on the board across a reset (`pvr_vkhang`) or under the shim (below). Each frame it:
1. clears the colour and depth/stencil buffers;
2. draws a triangle in immediate mode;
3. flushes;
4. reads one packed depth/stencil value back.

Mesa reads packed depth/stencil by mapping the buffer directly, so the read waits for the batch the flush ended, as the present does. The program must run to the end and print `PASS`.

**Thread-local storage after a library is unloaded.** Haiku's runtime loader starts a new thread's dynamic thread vector at generation 0. That breaks once a library with thread-local storage has been unloaded in the process, for example when zink closes the Vulkan driver at `eglTerminate()`. From then on, a new thread's second TLS access frees and re-creates the blocks of every library loaded since, and what its first access wrote is lost.

Summit's WebProcess crashed this way. Zink's shader-cache thread wrote `util_call_once_data()`'s context into the driver's TLS. The `call_once()` callback read it back as NULL and called it (`PC 0`, under `vk_pipeline_cache_create()` from zink's `cache_get_job()`).

- The real fix belongs in `src/system/runtime_loader/elf_tls.cpp`: start a new vector's counter at the current generation.
- `tls_generation_check` tests the runtime loader. It loads, unloads and reloads `libtls_generation_check.so`. Then a new thread writes the library's TLS and reads it back. It should fail on the current loader and pass once the loader is fixed.
- Until the loader is fixed, `mesa-haiku-gl.patch` has every thread Mesa creates touch its TLS twice before it runs anything. Application threads that make a GL context current are still exposed if those are their first TLS accesses.

**Readback from cached memory.** Each GL frame's readback copies the image into a zink staging buffer, then the CPU reads it. That covers `glReadPixels` and the BGLView present. Zink's staging heap only takes types that are HOST_COHERENT and HOST_CACHED. With no such type, it used the write-combined coherent one: about 7 ms for 300x300 on the board. `mesa-haiku-gl.patch` changes that:
- Without a coherent cached type, the staging heap takes the HOST_CACHED types first. This is on by default only on Haiku: `ZINK_NONCOHERENT_CACHED_STAGING=0`/`1` overrides it.
- Resources with coherent or persistent maps skip non-coherent types, because those maps are never flushed or invalidated.
- A read map of such a buffer is read directly, with no second staging copy.
- `zink_image_map()` invalidates a non-coherent staging buffer or linear image before it is read. It never did, on any OS. The invalidate covers only the object's own bytes.
- Writes are flushed at unmap, as zink already did for non-coherent memory.

Under the shim, `pvr_glbench` allocates its staging slab from type 1 and makes one 360000-byte invalidate per frame (300x300x4). The readback time there means nothing, because host memory is cached either way.

Zink keeps its shader cache in the usual Mesa cache directory, limited to 128 MB (`MESA_SHADER_CACHE_DISABLE=true` turns it off). Its key includes the library file's timestamp.

**Why `libvulkan.so.1` is a shim.** There is no Vulkan loader on arm64 Haiku. Zink `dlopen()`s `libvulkan.so.1` and takes `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` from it. `vulkan_shim.c` provides those two, plus the global commands, by forwarding to the driver's `vk_icdGetInstanceProcAddr`. It links the driver by name. This keeps zink unpatched and gives any program the usual way in. It is not a loader: no layers, one driver, no window system surfaces. A ported Khronos loader would replace it.

`pvr_glprobe [--expect TEXT] [--ppm FILE] [--repeat N]` checks GL on the board in one command:
- It prints the EGL vendor and version, then `GL_RENDERER`, `GL_VERSION` and `GL_SHADING_LANGUAGE_VERSION`.
- It clears a 64x64 pbuffer, then draws the same triangle reference as `pvr_vktriangle` (constant colour, no pixel centre on an edge), and checks every byte read back. A second frame repeats the check.
- `--expect zink` fails the run unless the renderer is zink, as in `zink Vulkan 1.2(PowerVR B-Series BXM-4-64 MC1 (IMAGINATION_OPEN_SOURCE_MESA))`.
- `LIBGL_ALWAYS_SOFTWARE=1 pvr_glprobe --expect softpipe` checks the fallback.
- `--repeat N` does all of it N times in one process, `eglTerminate()` included. Zink unloads the Vulkan driver with its screen and loads it again for the next one. On an unfixed runtime loader, the second run is what crashed Summit's WebProcess (below).

## Host smoke test (`build.sh shim`)

`build.sh shim` builds the same patched tree for Linux; the Haiku hunks compile out. It adds Mesa's pvr drm-shim (`PVR_SHIM_DEVICE_BVNC=36.56.104.183`) and `pvr_ioctl_trace.c`, an `LD_PRELOAD` tracer. It then runs the Vulkan tests (`pvr_vkhang` with its default 180 s timeout, harmless under the fake device, and `pvr_vktriangle --linear`) and `pvr_glprobe` on the build host.

All of the driver's own code runs: enumeration, device creation, the PowerVR compiler on this core, command streams and null jobs. Every DRM ioctl lands in `shim/<test>.log`, in order and with its flags, so the logs show what the kernel will receive. The differences on Haiku are that `GET_BO_MMAP_OFFSET` + `mmap()` become `MAP_BO`, and enumeration opens the node by path.

Every test runs with 256 KiB stacks (`ulimit -s 256`), Haiku's default for an application's thread. A stack frame too large for the board crashes here too; this is how the `pvr_queue_transfer()` overflow reproduces off-board. The runs set `PVR_CACHED_MEMORY_TYPE=1` and `ZINK_NONCOHERENT_CACHED_STAGING=1`, so they get the board's memory types and zink's staging in the cached one. Host memory is coherent, so the cache maintenance does nothing there.

The shim executes nothing. So in the expected results:
- `pvr_vkprobe` passes;
- `pvr_vkfill` reports every word unwritten, in both modes;
- `pvr_vkfence` reports the 9 checks that need real syncobj state (values, `NOT_READY`, timeouts).
- `pvr_vktriangle` (both modes) reports every pixel never written.
- `pvr_vkhang` runs its whole flow in milliseconds and reports FAIL. The fake device signals the fence at once, so the "hang" ends instantly (no 180 s wait off-board) and the recovery dispatches find their data unwritten. Its value here is the ioctl trace: a COMPUTE `SUBMIT_JOBS` then a bounded `SYNCOBJ_WAIT WAIT_FOR_SUBMIT` on the job's syncobj, the timeout the kernel will see.
- `pvr_glprobe --expect zink` runs zink on the PowerVR driver through Linux EGL (surfaceless, the shim's render node, `MESA_LOADER_DRIVER_OVERRIDE=zink`). It prints the zink renderer and OpenGL ES 2.0, links the program, and submits render and transfer jobs. The pixels it reads back are zeros.
- `tls_generation_check` passes: glibc's TLS has no such bug.
- `pvr_glprobe --expect zink --repeat 3` runs the probe three times in one process.
- `pvr_glreset` passes.
- `pvr_vkbench` (fence; then `--timeline --rerecord`) and `pvr_glbench` each run for 3 s with 1 s lines and fail their pixel or word checks. On the host their rates measure the driver and zink CPU paths only, because the shim executes nothing.

The tracer also logs CPU maps of buffer objects: `mmap` of the DRM device and `munmap` of such a map, as `CPU_MAP` and `CPU_UNMAP` lines. On air/OS these are `MAP_BO` and `delete_area()`. Three marked runs, `pvr_glbench --frames 60 --mark`, `pvr_glbench --frames 60 --resize 5 --mark` and `pvr_vkbench --dispatches 60 --timeline --rerecord --mark`, are cut by `trace_balance` into frames 10 to 59. For each kind of object, it reports how many are made and freed per frame, and the net count for each half of that window. A kind whose net count grows in both halves is marked `PILES UP`. The result is in `shim/balance-*.txt`. Nothing piles up. With the data set cache, a steady GL frame makes no free list and no HWRT data set: two of each are made in the first frames (zink keeps two batches in flight) and reused after that. In the resize run, each new size makes one data set the first time, and none after that. A GL frame still makes, and frees again within that frame:
- 2 buffer objects, both CPU mapped: the graphics sub-command's control stream (`pvr_arch_csb.c`) and the render's SPM background-object constants (`pvr_arch_spm.c`). Before the cache, it was 7 buffer objects, 1 free list and 1 HWRT data set;
- 4 GPU VM maps (before: 9);
- 13 syncobjs, over 3 `SUBMIT_JOBS` with 5 jobs.

On the board, each of these is still kernel work every frame: areas and MMU flushes, but no firmware objects now.

Then come three GPU-reset runs: `lost-pvr_vkfill`, `lost-pvr_glprobe` and `lost-pvr_glreset`. `PVR_TRACE_FAIL_SUBMIT=N` makes the tracer fail the N-th `SUBMIT_JOBS` and every later one with `EIO`, as the kernel does after a reset; the shim is not called. N is 1 for the first two runs and 10 for `pvr_glreset`, so the loss comes in the middle of its frames. Each run is given 60 s. The expected results:
- `lost-pvr_vkfill` exits 1 with `vkQueueSubmit ...: -4` (`VK_ERROR_DEVICE_LOST`). The trace shows `[injected EIO]`, then a `SYNCOBJ_SIGNAL` of the fence's syncobj. The drm-shim does not implement that request (`unhandled core DRM ioctl 0xC5`), but the kernel driver does.
- `lost-pvr_glprobe` exits 1 with `FAIL` within milliseconds, after zink's `VK_ERROR_DEVICE_LOST` line. The trace shows the one failed `SUBMIT_JOBS`, then a `SYNCOBJ_TIMELINE_SIGNAL` of zink's batch timeline point, then no more submits. A hang shows up as exit 124 marked `(HUNG)`.
- `lost-pvr_glreset` runs all 300 frames, and its teardown, and exits 0 with `PASS`. With the original `zink_batch.c` it hung (exit 124). gdb showed the main thread in `zink_image_map()` → `zink_resource_usage_wait()` → `zink_batch_usage_unflushed_wait()` → `cnd_wait()`.

The shim's waits never block, so a host run cannot reproduce a thread that is already asleep in the kernel when the submit fails. On the board, the signal after the loss is what wakes that thread.
