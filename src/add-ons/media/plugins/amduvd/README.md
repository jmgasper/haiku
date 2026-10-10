# AMD UVD video components

This directory currently supplies `H264Stream`, the userspace metadata layer
for the native WX5100 UVD interface. It is not yet an installed Media Kit
add-on. The kernel interface and native acceptance results are documented in
`docs/x399-workstation/WX5100.md`.

`Prepare` takes one Annex B access unit, parses parameter sets and slice
headers, produces a slice-only bitstream and an `amdgpu_h264_picture`, and
computes picture order. Call `Commit` only after successful hardware decode.
On a decode error or seek, discard pending output, destroy the kernel session
and call `Reset` before resuming at an IDR. Parameter sets survive `Reset` so
that a container's existing codec configuration can still be used. Each
decoder has its own parameter sets and reference state.

The current path supports progressive eight-bit 4:2:0 Baseline/Main/High
metadata. It rejects field pictures, other chroma/bit-depth combinations and
transform bypass. The driver independently checks configuration, metadata,
input extents, NAL types, handles and ownership; this userspace parser is
not that trust boundary. Allocation or unsupported-format errors must feed
the application's software fallback. No additional codec capabilities are
advertised merely because metadata can be parsed.

`jam amdgpu_video_stream` builds a native test client. The adjacent test tool
`src/tests/add-ons/kernel/drivers/amdgpu/video_stream.py` packages raw H.264
into access-unit records with `--pack`, then verifies native `.frames` output
with `--frames`. `amdgpu_video_stream --parse input.au` checks only metadata;
`amdgpu_video_stream input.au output.frames --unprivileged` decodes through
the driver as UID 65534. The output is a diagnostic record stream, not a media
container. FFmpeg reference decoding must preserve one output picture per
coded picture (`-fps_mode passthrough`) and disable cropping to compare the
complete coded surface.

The next integration work is a timestamped output/reordering queue, AVCC
conversion and codec configuration, the Media Kit decoder, and explicit
selection/fallback in airTime and Summit. Predictive multi-client playback,
seek/flush, format changes and error handling need their own native tests.
