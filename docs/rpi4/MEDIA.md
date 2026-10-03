# Media on the Raspberry Pi 4

State 2026-10-04: airTime (`/mnt/HaikuWork/apps/airTime`, the arm64 package
built for the ROCK 5) runs unchanged and decodes in software with the image's
ffmpeg package. There is **no hardware decoding and no sound output** yet.

## Checked on the board

| Clip | Decoder | Result |
|---|---|---|
| `multi.mkv`: H.264 1080p30, AAC/AC-3, subtitles | libavcodec | 30 fps shown (19 pictures dropped while starting), A/V offset 0 ms, decode 0.1 ms per picture in the player thread (frame threads do the work), compose 17 ms, draw 3.6 ms; picture on screen (`evidence/airtime1.jpg`) |
| `hevc8.mp4`: HEVC 1080p30 8 bit | libavcodec | 18.8 fps, 43 ms per picture: too slow in software |

Numbers are airTime's own (`hey application/x-vnd.airOS-airTime get Stats of
Window 0`). airTime's log says "RK3588 VPU refused the stream" before it
falls back; that is its hardware add-on's name, the add-on is not on this
board.

The airTime package is in the `rpi4-airos` image profile.

## Open

- **Sound.** `/dev/audio/hmulti` is empty: there is no driver for HDMI audio
  or the headphone jack. Both are services of the VideoCore firmware reached
  through VCHIQ, which is not ported. A USB sound card should work with the
  existing `usb_audio` driver (none is attached in the lab).
- **Hardware decoding.** H.264 goes through the firmware's codec service
  (again VCHIQ, "MMAL"); HEVC through the SoC's own decoder block (stateless,
  the bit stream parsing is the driver's or the player's). Neither is
  started. A VCHIQ port (FreeBSD has a BSD-licensed one) is the piece that
  unlocks HDMI audio and H.264 together.
- Presentation is a copy into the window's frame buffer (17 ms compose at
  1080p); the firmware compositor could take a video plane directly
  (`rpi_display` uses the same mechanism for the desktop).
