# Media on the Raspberry Pi 4

State 2026-10-04: sound output and hardware H.264 decoding work, both through
the VideoCore firmware. airTime plays H.264 on the firmware's decoder with
sound. Nobody has listened to the sound: the lab has no ears (see "How the
sound was checked").

| Piece | What it is | Where |
|---|---|---|
| `vchiq` | the message channel to the firmware's services, a kernel module | `src/add-ons/kernel/generic/vchiq` |
| `/dev/misc/vchiq` | the same services for user programs | `src/add-ons/kernel/drivers/misc/vchiq.cpp` |
| `bcm2835_audio` | sound driver (multi audio): HDMI 0, HDMI 1, headphone jack | `src/add-ons/kernel/drivers/audio/bcm2835` |
| `rpi_mmal` | Media Kit decoder add-on: H.264 on the firmware's decoder | `src/add-ons/media/plugins/rpi_mmal` |
| airTime 1.0.0-13 | knows `rpi_mmal`, takes its I420 pictures | `/mnt/HaikuWork/apps/airTime` |

All of it is in the `rpi4-airos` image; `config.txt` gives the firmware
`gpu_mem=192` for the decoder.

## VCHIQ

The firmware's services (sound, the multimedia components, and more) are
reached through VCHIQ: slots of shared memory that each side fills with
messages, a doorbell register, and bulk transfers where the firmware copies
from or to ARM memory itself. `vchiq.cpp` is a new implementation of the ARM
side of Broadcom's protocol (their implementation is BSD licensed and was the
reference). It leaves out what nothing here needs: services opened by the
firmware, synchronous services, suspend, message quotas.

- The module finds the `brcm,bcm2711-vchiq` node in the device tree itself;
  on another board it loads and reports `B_DEVICE_NOT_FOUND`, and the two
  drivers on top of it publish nothing.
- Kernel API in `headers/private/drivers/vchiq.h`: open a service with a
  hook that gets its messages, queue a message, bulk transmit and receive.
- `/dev/misc/vchiq` has the same as ioctls; a service is closed with the file
  descriptor that opened it, so a program that dies leaves nothing open.
- Bulk transfers go through a bounce buffer per service and direction
  (memory in the first gigabyte, CPU cache flushed around the firmware's
  access). That costs a copy; see "Open".

Two things cost time and are worth knowing:

- On the BCM2711 the page list of a bulk transfer holds 36 bit physical
  addresses shifted right by four, with the run length in eight bits. The
  form of the older boards (bus address, twelve bits of run length) is what
  mainline Linux has; the firmware cancels every transfer written that way.
- The firmware cancels a bulk transfer whose size is not what it expects.
  For the decoder's input that is the length rounded up to four bytes.

## Sound

`bcm2835_audio` hands the firmware's sound service ("AUDS") 16 bit stereo at
44.1 or 48 kHz in buffers of 960 frames. The firmware does the rest: the
audio packets of the HDMI outputs, the PWM of the headphone jack. It reports
what it has played in pieces of about 10 ms; the driver sends the next buffer
when the firmware is down to one, and that pace is the Media Kit's clock.
Mixer controls: volume (0 to -60 dB) and output (automatic, headphone jack,
HDMI 0, HDMI 1); "automatic" is the firmware's choice.

### How the sound was checked

Nobody can listen in the lab, so this is what stands in for ears:

| Check | Result |
|---|---|
| The Media Kit's default output | "Raspberry Pi sound output" |
| `rpi4_tone` (a BSoundPlayer tone through the system mixer), several runs of 3 to 12 s | the mixer asks for 48000 frames per second; the driver's own count is 48000 frames in 1.000 s, second after second, with no underrun after the start |
| `rpi4_vchiq_audio <destination> 2` (a tone straight to the service) for the jack, HDMI 0 and HDMI 1 | the firmware takes 48000 frames per second on each |
| the lab KVM's HDMI receiver on HDMI 0 (its driver logs an interrupt when the audio of the signal changes) | logs when the sound service is pointed at HDMI 0 and again when it is closed or pointed at the jack; nothing while the jack plays. Through the driver, which keeps the service open, that is at each change of the "Output" control |
| `rpi4_tone controls` | volume and output can be read and set through the Media Kit |
| airTime, a film with AAC sound | audio and video stay within about 20 ms of each other (`av=` in its Stats) |

So the samples reach the firmware at the right rate, and HDMI 0 carries an
audio stream when the output is HDMI 0 and none when it is the jack. What it
sounds like, and whether anything comes out of the jack or HDMI 1, is not
checked. (The monitor on HDMI 1 is detected since 2026-10-04, with its EDID;
whether it has speakers is not known here.)

## Hardware decoding

The firmware's H.264 decoder is the component `ril.video_decode` of its
"mmal" service. `MmalDecoder` speaks that service's messages over
`/dev/misc/vchiq`: Annex B byte stream in, in pieces of at most 80 KiB;
pictures out in display order as I420 with the time stamps that went in.
`rpi_mmal` wraps it as a decoder add-on of the Media Kit.

- H.264 up to 1920x1088, eight bit 4:2:0 (what the firmware decodes).
- The add-on registers no format with the Media Kit: the lookup would give
  it every H.264 stream and has no fallback when it refuses one. A player
  loads it by name; airTime does, and goes to libavcodec when it refuses.
- Pictures are handed out as 'I420' (three planes) or 'NV12', both colour
  spaces Haiku has no constant for; see the top of `RpiMmalPlugin.cpp`.
- At most two decoders at a time in the whole system (a named port per
  decoder is the token). The firmware's memory is `gpu_mem`; when it runs
  out the firmware's mmal service stops answering until the next start. That
  happened with the default 76 MB and two 1080p decoders. With 192 MB four
  ran side by side.

### Checked on the board

| Check | Result |
|---|---|
| `rpi4_mmal_decode 1920 1080 test90.h264 - \| md5sum` (90 pictures of 1080p High profile, cut from `multi.mkv`) | the same md5 as `ffmpeg -i test90.h264 -pix_fmt yuv420p -f rawvideo -` on the build host: every picture byte for byte |
| the same without writing the pictures | 60 to 70 pictures per second |
| two decoders at once | both give 90 pictures |
| eight decoders killed (`kill -9`) in mid-stream, then one more | byte exact again: the firmware frees what a dead client had |
| airTime, `multi.mkv` (H.264 1080p30 High, AAC) | "Raspberry Pi VideoCore (rpi_mmal h264) [hardware]", the picture is right (`evidence/airtime-hw1.jpg`), sound in step; seeking, playing to the end and seeking back from there work |
| all of the above but the kill test, on a card flashed from the `rpi4-airos` image | the same |

### What it buys

Measured with airTime on `multi.mkv`, 1080p30 in a window, with sound
(`top`, 4 s; airTime's Stats):

| | hardware | software (libavcodec) |
|---|---|---|
| CPU for decoding | about a quarter of a core (the add-on's reader thread and airTime's decoder thread) | about 60 % of a core (four frame threads) |
| shown | 27 to 29 pictures per second (27.7 over 30 s on the flashed image) | 29.8 |

The pictures on screen do not get more: turning a picture into pixels
(airTime's scaler, 20 to 25 ms) is the larger cost on this board, and one
core is lost to the Wi-Fi driver (below). After a seek the hardware path is
slower to catch up: it decodes from the key frame at 60 to 70 pictures per
second and every picture is copied out.

## Lab tools

In the lab image's `~/config/non-packaged/bin`:

- `rpi4_vchiq_audio <destination> <seconds> [Hz] [dB]`: a tone straight to
  the firmware's sound service.
- `rpi4_tone <seconds> [Hz]`: a tone through the Media Kit;
  `rpi4_tone controls [id value]` lists and sets the sound driver's mixer
  controls.
- `rpi4_mmal_decode <width> <height> <file.h264> [output | -]`: the
  firmware's decoder on an Annex B stream.

A kernel driver is replaced with `tools/rpi4/install-driver.sh` (it
restarts the board). Swapping the sound driver's file under the running
media server ended in a panic.

## Open

- **Nobody has heard it.** Nothing is known about the jack and HDMI 1
  beyond the firmware taking the samples; choosing the output in the Media
  preferences by hand is untried (the controls answer to
  `rpi4_tone controls`).
- **HEVC.** The SoC's HEVC block is not driven by the firmware; it needs a
  driver of its own that parses the stream (Linux: `rpivid`). HEVC 1080p
  stays at about 19 fps in software.
- **Copies.** A picture is copied twice on its way (bounce buffer to the
  add-on, add-on to the player). Page lists of the caller's own pages and
  receiving straight into the player's buffer would remove both.
- **Presentation.** airTime still converts and scales on the CPU; a video
  plane of the firmware's compositor would take the decoder's pictures as
  they are.
- **Other players.** MediaPlayer and the Media Kit's own lookup do not use
  the hardware decoder (see above).
- **Fewer pictures than software.** The hardware path shows 27 to 29
  pictures per second where libavcodec shows 29.8, although it uses less
  than half the CPU for decoding; airTime's conversion to pixels takes 4 to
  5 ms longer per picture then. Why is not found.
- **Wi-Fi driver.** `bwfm sdio poller`, the thread of `broadcomfmac` that
  serves the card, uses a whole core all the time, also when nothing is
  joined and with the interface down (seen in `top` while measuring here).
  Not looked into yet; it costs every program on the board a quarter of
  the CPU.
