# Mesa's PowerVR Vulkan driver for air/OS (Cubie A7S)

`build.sh` cross-builds Mesa 26.2.4's Imagination Vulkan driver,
`libvulkan_powervr_mesa.so`, for Haiku arm64, and three test programs.
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
| `configure`, `driver` | Cross-builds only `-Dvulkan-drivers=imagination`: no GL, EGL, LLVM, shader cache or WSI. It uses the ROCK 5's pinned sysroot and cross file (`artifacts/mali-system-opengl-build/20260918T125722Z`), as the Pi 4 build does. |
| `tests` | Builds the test programs against the driver. Linking also checks that every symbol the driver needs exists in the sysroot. |
| `all` | Runs everything above and fills `out/`. |

Everything lives under `/mnt/HaikuWork/cubie/mesa` (`CUBIE_MESA_ROOT`):
`downloads/`, `mesa-26.2.4/` (patched), `host-tools/bin`, `build-host/`,
`build/`, `tests/`, and `out/`.

`out/` holds:
- `libvulkan_powervr_mesa.so`, stripped (`debug/` has the unstripped copy);
- `pvr_vkprobe`, `pvr_vkfill`, `pvr_vkfence`;
- `powervr_mesa_icd.aarch64.json`, for a loader later;
- `MANIFEST`, with the input hashes.

## The patch

The header of `mesa-haiku-pvr.patch` lists each piece. In short:
- `include/xf86drm.h` stands in for libdrm. `drmIoctl()` becomes `ioctl(fd, PVR_HAIKU_OP(request & 0xff), arg, IOCPARM_LEN(request))`, and there are libdrm-style syncobj wrappers.
- `src/util/u_sync_provider_haiku.c` connects those wrappers to the Vulkan runtime, with timelines.
- `pvr_instance.c` opens the device node directly. `HAIKU_PVR_DEVICE` overrides the path.
- `pvr_drm_bo.c` maps buffers with `PVR_HAIKU_NR_MAP_BO` and unmaps them with `delete_area()`.
- Small fixes cover `drm.h`, `pvr_drm.h`, `vk_image` and `pvr_physical_device.c`.

There are no buffer or sync file descriptors yet. Those paths fail with `EOPNOTSUPP`. They forward to the kernel once `pvr_haiku.h` defines `PVR_HAIKU_NR_PRIME_*` or `PVR_HAIKU_NR_SYNCOBJ_{HANDLE_TO_FD,FD_TO_HANDLE}`.

To change the patch:
1. Edit the tree under `$ROOT/mesa-26.2.4`.
2. Diff it against the pristine unpack in `$ROOT/pristine/mesa-26.2.4`: `diff -ruN -x __pycache__ -x '*.pyc' -x '.haiku-pvr.*'`, with paths rewritten to `a/`, `b/`.
3. Keep the header.
4. Write the new patch's sha256 to `$ROOT/mesa-26.2.4/.haiku-pvr.sha256` and copy the patch to `.haiku-pvr.patch` next to it. Otherwise `build.sh` reverts the old patch and applies the new one over your edits.

What the kernel has to match:
- **Syncobj wait timeouts** are absolute `CLOCK_MONOTONIC` nanoseconds, as on Linux; on Haiku that is `system_time() * 1000`. `INT64_MAX` means wait forever.
- **A wait that runs out of time** must fail. Userland maps `B_TIMED_OUT` and `B_WOULD_BLOCK` to `ETIME`, and does not retry waits on `EAGAIN`.
- **Every other request** is retried on `EINTR`/`EAGAIN`, as libdrm does.

## Tests

All three need `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1`, because 36.56.104.183 is not on Mesa's conformance list. There is no Vulkan loader on arm64 Haiku, so each program links the driver directly and starts from `vk_icdGetInstanceProcAddr`. Put `libvulkan_powervr_mesa.so` in `/boot/system/non-packaged/lib`, or next to the program with `LIBRARY_PATH=%A:$LIBRARY_PATH`.

- `pvr_vkprobe` prints the instance, the properties (name, versions, IDs, UUIDs, DRM nodes), the limits, the 1.0/1.1/1.2 features, the memory heaps and types, the queue families and the device extensions. It then creates a device with one queue, waits for it to go idle and destroys it. `PVR_DEBUG=info` also makes the driver dump the core's details.
- `pvr_vkfill [runs] [timeout-ms]` runs one compute dispatch that writes `gl_GlobalInvocationID.x * 3 + 1` into a 1 MiB storage buffer in HOST_VISIBLE|COHERENT memory. The buffer is prefilled with `0xdeadbeef`. The test waits on a fence, checks every word, and prints the first 16 mismatches, the count and the timings.
    - The shader is SPIR-V 1.0 in the source as words. There is no glslang anywhere in the lab, so it was written in SPIR-V assembly (quoted in the source), assembled with `spirv-as` and validated with `spirv-val --target-env vulkan1.0`, both from `toolchains/mesa-native-deps/usr/bin`.
- `pvr_vkfence [timeout-ms]` submits with no command buffers, which takes the null-job path. That submit signals a fence and timeline value 1. The test then:
    - waits on both, and checks the counter;
    - submits again, waiting on 1 and signalling 2;
    - signals 3 from the host;
    - checks that a wait for 10 and a wait on a reset fence each return `VK_TIMEOUT` after 10 ms.

Every program prints `PASS`/`FAIL` and exits 0/1. Output is line-buffered, so it survives a crash.

## Host smoke test (`build.sh shim`)

`build.sh shim` builds the same patched tree for Linux; the Haiku hunks compile out. It adds Mesa's pvr drm-shim (`PVR_SHIM_DEVICE_BVNC=36.56.104.183`) and `pvr_ioctl_trace.c`, an `LD_PRELOAD` tracer. It then runs the three tests on the build host.

All of the driver's own code runs: enumeration, device creation, the PowerVR compiler on this core, command streams and null jobs. Every DRM ioctl lands in `shim/<test>.log`, in order and with its flags, so the logs show what the kernel will receive. The differences on Haiku are that `GET_BO_MMAP_OFFSET` + `mmap()` become `MAP_BO`, and enumeration opens the node by path.

The shim executes nothing. So in the expected results:
- `pvr_vkprobe` passes;
- `pvr_vkfill` reports every word unwritten;
- `pvr_vkfence` reports the 9 checks that need real syncobj state (values, `NOT_READY`, timeouts).
