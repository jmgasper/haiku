# RK3588 hardware media decoding

The ROCK 5 ITX image includes hardware decoding for 8-bit H.264, H.265 and
AV1 video. The Media Kit decoder add-on uses Rockchip MPP and the in-tree
`rk3588_vpu` driver. `Rock5MediaPlayer` is a small sample application with
video, queued audio, play, pause and seek controls.

The qualified paths are:

| Format | RK3588 block | Media Kit input | Qualified output |
| --- | --- | --- | --- |
| H.264/AVC | RKVDEC0 (VDPU381) | MP4, converted to Annex B | NV12, then B_RGB32 |
| H.265/HEVC | RKVDEC0 (VDPU381) | MP4, converted to Annex B | NV12, then B_RGB32 |
| AV1 | VPU981 | MP4 with temporal delimiter insertion | NV12, then B_RGB32 |

This qualification covers the 1280x720, 8-bit Big Buck Bunny samples below.
Other profiles, bit depths, chroma layouts and containers need separate tests.

## Reproducible build

`build-ffmpeg-arm64.sh` builds and packages:

- FFmpeg 6.1.6 from the release archive with SHA-256
  `d4fcb164028dd3beee5d92c0ac72e46aac6973c75ea12dc14de07bf8f407370a`;
- Rockchip MPP `develop` commit
  `14729dd578e570e5f00fd1dd2113f5429012d64b` with
  `mpp/mpp-14729dd-haiku.patchset` (SHA-256
  `04dc008c41f397624121102743fe357f7663f9a7213195e22f9e8b6be49ec201`);
- Haiku's regular FFmpeg Media Kit add-on;
- `00_rockchip_mpp`, the RK3588 Media Kit decoder add-on;
- `librockchip_mpp.so.1`; and
- `Rock5MediaPlayer`.

Run the build from the Haiku source directory:

```sh
source tools/rock5-itx/env.sh
tools/rock5-itx/build-ffmpeg-arm64.sh
```

The resulting `rock5_ffmpeg-6.1.6-2-arm64.hpkg` is added to
`@rock5full-mmc`. The qualified package SHA-256 is
`bbca9a19d039d9a58430d2f47e33df485ab050e9a16c0e29145d7631b78a0540`.

## Driver safety model

The driver accepts MPP buffer allocation and decode requests from applications.
Power and register diagnostic requests require a privileged opener. Every
decode job:

1. validates the exact RK3588 block identity and the selected board profile;
2. validates all buffer handles, ownership, ranges and register addresses;
3. powers the required parent and child domains and configures their clocks;
4. submits one bounded job and polls completion for at most 500 ms; and
5. restores the exact PMU and CRU state, including failure and timeout paths.

The installed default settings enable the profiles qualified with EDK2 v1.1:

```text
power_profile rock5-itx-edk2-v1.1-vdpu-power
rkvdec0_profile rock5-itx-edk2-v1.1-rkvdec0-power
av1_profile rock5-itx-edk2-v1.1-av1-power
decode_profile rock5-itx-edk2-v1.1-rkvdec0-h264-h265
av1_decode_profile rock5-itx-edk2-v1.1-vpu981-av1
```

Host tests cover successful jobs, rejected state and addresses, timeouts and
restoration. They finish with `ROCK5_VPU_POWER_TEST_PASS`.

## Native hardware qualification

The source Big Buck Bunny samples live outside Git in
`/mnt/HaikuWork/artifacts/media-samples/`:

| Sample | SHA-256 |
| --- | --- |
| `bbb-20s-h264-aac.mp4` | `ef26380aa96bc1685c484a32f43e2163b86e6a692fad4a746c5fd8fc43f4a649` |
| `bbb-20s-h265-aac.mp4` | `074af9165f6813a2f3a93440e6497e52b6270e5ba4e0e0d1969863597bec959a` |
| `bbb-20s-av1-aac.mp4` | `68d8585d8dd704b7bdb7583e001f64e35f6ca6eaa2b6516dd54a877b90b14451` |

Direct MPP jobs produced byte-for-byte identical NV12 frames to host FFmpeg:

| Format | Hardware/software frame SHA-256 | PSNR |
| --- | --- | --- |
| H.264 | `d14068d66a82caede48a6292c79e5f5a7bc1b53790c303369b646138e7c973c2` | infinite |
| H.265 | `fdfa2bf4dc557ce0d570df8d17073712157e79acb55904d47a6924d032d9928d` | infinite |
| AV1 | `758c2896a527e20367cc47ccb8e4379761451a6d79368e4e245ce9b03cfc7a99` | infinite |

The Media Kit probe decoded the first video and AAC audio buffers, sought to a
later keyframe, and decoded both tracks again for all three formats. The sample
player continuously played AV1 with its audio path active, paused, sought to
about 10 seconds while paused, and resumed with a new video scene.

Native evidence is outside Git under
`/mnt/HaikuWork/artifacts/ffmpeg-arm64/native/`. The principal records are:

- `test-media-mpp-h264-v6.txt`, `test-media-mpp-h265.txt` and
  `test-media-mpp-av1.txt` for Media Kit decode and seek;
- `player-continuous-h264.txt` and `player-continuous-h265.txt` for continuous
  hardware frames;
- `check-hardware-player-av1-v7.txt`,
  `player-hardware-av1-v7-running.jpg` and
  `player-hardware-av1-resumed.jpg` for player controls and playback;
- `item7-final-media-probes.txt` for post-install H.264 and AV1 decode and seek;
- `item7-final-power-restored.txt` for the final all-domains-off snapshot; and
- `item7-final-nvme-boot-serial.log` for the installed NVMe reboot.

The final full image is
`/mnt/HaikuWork/artifacts/images/haiku-arm64-b37ec067153587c0.img`, SHA-256
`b37ec067153587c08c35ce0ddc28153ac14c828fbfd34cf91c6156603578ef77`.
It passed QEMU in `artifacts/qemu/20260922T194046Z-e3f870`. The final driver,
SHA-256
`ce0343a9acfe5727b37fdf91d562b02fb01b8f9eb45f6e351a3a66b98969aa4d`,
and package were installed on the NVMe system. A serial-captured reboot and
post-install tests passed. The final PMU snapshot reported RKVDEC0, RKVDEC1,
VDPU and AV1 off after decoding.

The player's `BSoundPlayer` path and AAC decode are working. Physical audio
output qualification is tracked separately with the board audio work.
