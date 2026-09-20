# The graphics card's video decoder, as a media add-on

The GeForce cards this fork runs on have a video engine - NVDEC - that decodes
H.264 without the processor doing any of the arithmetic. This add-on offers it
to Haiku's media kit, so that any program that plays video gets it: the file is
opened as usual, and the pictures come back from the card.

| | |
| --- | --- |
| `nvdec_engine.c` | The engine itself: a resman client, an address space, buffers both sides can see, and a channel on the decoder with an NVC2B0 object on it. Also where the card's arrangement of pixels in memory is written down. |
| `h264_parse.c` | Sequence and picture parameter sets, and as much of a slice header as says which picture a slice belongs to. Nothing here touches picture data: the engine parses every entropy-coded bit itself. |
| `nvdec_h264.c` | The bookkeeping the engine does not do - which pictures are still needed as references, what order they are shown in, which surface each one is in. |
| `nvdec_convert.c` | Out of the card's arrangement and into pixels, in bands, across as many processors as there are. |
| `NVDecPlugin.cpp` | The media kit's side of it. |

## What it does and does not decode

Eight bit 4:2:0 H.264, progressive, up to what the engine allows - 4096 wide on
this generation. Field pictures, 4:2:2, 4:4:4 and more than eight bits a sample
are refused rather than decoded wrongly.

Haiku picks one decoder for a format, by add-on directory, so this add-on takes
H.264 away from libavcodec wherever it is installed. A stream it refuses will
not fall back: it will not play. That is the reason for refusing narrowly and
loudly rather than trying.

## Building it

It is not built with the rest of the tree, because it needs NVIDIA's resource
manager headers, which live in the NVK checkout on the workstation and are not
part of this repository. `docs/x399-workstation/tools/build-nvdec-plugin.sh`
builds it there and installs it into the user's non-packaged add-ons.

## What was measured

Thirteen H.264 streams and 120 frames of 1080p Big Buck Bunny decode byte for
byte identically to ffmpeg's own decoder. 1080p decodes at 232 pictures a
second - 4.3 ms each - and with the conversion to pixels, a whole 1080p film
plays at 148 pictures a second through the media kit. `tests/nvdecstream.c`
does the comparison; `tests/mediadecode.cpp` goes through the media kit and
says which decoder answered.

Four things about the engine are not in the header that describes it, and each
cost a measurement to find. They are in the commit messages and in the comments
where they matter: the arrangement of pixels, the field markings that decide
whether a reference is a reference, the end of stream marker the bitstream
length has to count, and the reference table place a picture must keep.
