# AMD UVD Media Kit decoder

`jam amduvd` builds an explicitly selected Media Kit decoder over the native
WX5100 H.264 interface. It registers no formats with the Media Kit because
that lookup has no software fallback. An application loads `amduvd`, calls
`instantiate_plugin()` and `DecoderPlugin::NewDecoder(0)`, and retains its
software decoder when setup or decoding fails. Other decoder indices are
rejected. Application selection and release-image installation remain to be
implemented; the native tests load the add-on from a private lab directory.

The input is progressive eight-bit 4:2:0 Baseline/Main/High H.264, one complete
access unit per chunk. Both Annex B and MP4/Matroska AVC configuration and
length-prefixed packets are accepted. Packet expansion is limited to 4 MiB;
truncated prefixes, invalid parameter-set extents and unsupported formats
return errors. The kernel independently validates configuration, metadata,
bitstream extents, handles and ownership. Unsupported input is not silently
skipped.

`H264Stream::Prepare` parses parameter sets and every slice header, produces
a slice-only bitstream and an `amdgpu_h264_picture`, and computes picture
order. `Commit` updates references after hardware completion. This is
metadata handling, not entropy decoding. `Reset` discards reference state
and retains parameter sets. IDR and MMCO 5 begin separate output epochs;
MMCO 5 retains the original firmware POC and renumbers output/reference POC.
MMCO 5 is covered by metadata tests, not a native conformance bitstream.

`H264Output` holds at most 17 CPU pictures and emits them by output epoch
and POC, retaining each input timestamp exactly (including zero). It uses
the SPS reordering bound, drains at EOF and refuses inconsistent output
order. An interrupted chunk request does not drain pictures. `SeekedTo`
discards queued pictures and destroys the hardware session before a new
IDR. Same-size coded-format changes require an IDR and recreate the session;
a visible-size change returns `B_MEDIA_BAD_FORMAT` for caller renegotiation
or fallback. A malformed packet closes the session; a fresh `Setup` can
reuse the decoder.

Output is cropped, tightly packed `NV12` (custom colour-space value
`0x4e563132`), `I420` (`0x49343230`), `B_YCbCr422` (Haiku's Y0 Cb Y1 Cr), or
`B_RGB32`. YUV output preserves all samples. RGB conversion retains the
SPS full-range flag and supports BT.601/BT.709 matrices; an unspecified
matrix uses the usual SD/HD height convention, and other matrices fail.
The caller's negotiated output extent is checked before any copy.

The native `amduvd_plugin_test` exercises two independent predictive streams,
MP4 configuration, reordering/timestamps, EOF, seeks, interrupted input,
three YUV formats, malformed packets and reuse. `plugin_stream.py` packages
FFprobe packets and compares every output byte and timestamp against FFmpeg.
The final native run delivered 252 pictures across its stages, all exact.
`h264_output_test` checks conversion/cropping/guards, queue bounds/epochs,
AVCC and 25,000 malformed inputs; `h264_stream_state_test` checks metadata
reset and MMCO 5. These pass under host ASan/UBSan and in Haiku QEMU.

`amdgpu_video_stream` and its `video_stream.py` remain the lower-level kernel
stream tests; their `.frames` records contain the full coded surface rather
than Media Kit output. See `docs/x399-workstation/WX5100.md` for evidence.

Current performance is not qualified for playback. The kernel uses slow
per-word CPU accesses to clear/read back VRAM, and the interleaved 48-picture
320x240 plus 24-picture 1080p test took 20.12 seconds including output writes.
A DMA path to cached RAM, sustained performance, broader format/error
qualification, airTime/Summit integration, HEVC and final packaging remain.
