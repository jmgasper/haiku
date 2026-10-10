# AMD UVD Media Kit decoder

`jam amduvd` builds an explicitly selected Media Kit decoder over the native
WX5100 H.264 interface. It registers no formats with the Media Kit because
that lookup has no software fallback. An application loads `amduvd`, calls
`instantiate_plugin()` and `DecoderPlugin::NewDecoder(0)`, and retains its
software decoder when setup or decoding fails. Other decoder indices are
rejected. airTime and Summit now select it explicitly; full Summit browser
qualification and release-image installation remain pending. Native tests
and applications currently use the private lab build.

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
The native 12-picture Baseline/POC-2 P-picture MMCO-5 fixture matches FFmpeg;
other POC modes and B-picture MMCO-5 cases remain unqualified.

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

The kernel now clears VRAM and copies completed pictures to private cached
RAM through SDMA, with snooped GART mappings, RAM/VRAM guards, fenced engine
serialization and close-time reclamation. The same native 252-picture test
still matches every pixel and PTS; full coded-surface/padding checks pass too.
The interleaved 48-picture 320x240 plus 24-picture 1080p stage improved from
20.12 to 4.01 seconds including output writes (3.79 seconds to `/dev/null`).
This is a bounded regression measurement. Subsequent qualified ROM clock
requests allow the native three-minute 1080p30 airTime test to keep all
5,400 pictures on hardware, with 5,400 shown and no drops. Full Summit
browser/MSE qualification, broader format/error coverage and final packaging
remain; focused Summit file-decoder tests already pass.

The HEVC path is preparation and is not exposed by this addon yet.
`HevcStream` prepares Main/Main-10 UVD metadata, POC and retained references;
`HevcPacket` accepts hvcC configuration and length-prefixed packets, orders
parameter-set arrays for parsing and bounds expansion to 4 MiB. A failed
configuration disables packet conversion until reconfigured. Main and Main
10 fixtures pass parser, metadata, slot and packet-equivalence tests under
ASan/UBSan and in QEMU. `amdgpu_hevc_stream` and `hevc_stream.py` prepare
native NV12/P010 qualification, which has not run. These CPU checks do not
establish HEVC hardware decoding or Main-10 output quality.

`HevcOutput` preserves cropped P010 samples and converts RGB from the coded
precision and VUI range/matrix. Explicit eight-bit YUV output rounds ten-bit
codes to the nearest eight-bit value. H.264 and HEVC share the bounded
`VideoOutputQueue`, keeping per-picture format/depth and timestamps. The
output tests pass under ASan/UBSan and QEMU; the shared-queue H.264 addon
also passes the native 252-picture exact-pixel/timestamp regression.
