# NVDEC surface layout

The two scripts that worked out how the GTX 1080 Ti's video decoder lays out a
decoded picture in memory (`nvdec_convert.c` in
`src/add-ons/media/plugins/nvdec` implements the result):

- `deswizzle.py`: tries the standard 64x8 GOB swizzle with each pitch and
  block height on surfaces dumped off the card (`out-*.nv12`), against
  ffmpeg's decode of the same frame (`t1.yuv`), and prints the parameters
  whose luma matches.
- `layout.py`: the layout as it was measured: 512-byte groups of 64 columns
  by 8 rows, running down a block of `bh` groups, then across, then down the
  picture; `off()`, `untile()` and `plane_size()`.
