# ROCK 5 ITX analog audio

Item 8 was qualified on 2026-09-23 against the ES8316 codec wired to RK3588
I2S0 on the ROCK 5 ITX. The driver is installed by the regular ARM64 image and
publishes `/dev/audio/hmulti/rk3588/0` through Haiku's multi_audio interface.

## Implemented path

The kernel module admits only the `radxa,rock-5-itx` board and the enabled
`rockchip,rk3588-i2s-tdm` node at `0xfe470000`. It validates GIC interrupt 212,
then configures the audio power domain, CRU clocks, I2S0 pin mux, I2C7 and the
ES8316. The codec state and I2S routing follow register snapshots captured from
the same board under its Debian recovery system.

The current multi_audio endpoint provides stereo 16-bit playback at 48 kHz.
It uses two 1,024-frame shared buffers and services the I2S transmit FIFO from
interrupt 212. The I2S master clock is 12.288 MHz, BCLK is 3.072 MHz, and the
codec is unmuted only after the FIFO and synchronized transmit/receive clock
domains are running. Stop mutes the codec and leaves shared buffers valid until
the media client closes the endpoint.

The driver is added to the full image at
`system/non-packaged/add-ons/kernel/drivers/audio/hmulti/rk3588_audio`. Its
exported driver and device module names use that same path so Haiku can reload
the module after discovery.

## Verification

The host register, module-path, lock-order and buffer-lifetime checks pass:

```sh
python3 tools/rock5-itx/test_audio_registers.py
```

The focused ARM64 kernel add-on build and the complete regular image build
pass. The immutable full image is
`/mnt/HaikuWork/artifacts/images/haiku-arm64-d85128622cca9295.img`, SHA-256
`d85128622cca9295322cc50ce68680dd53d8272f283c75e050cb3ea84a2f0314`.
QEMU reached the framebuffer in both EL1 and EL2; retained evidence is in
`artifacts/qemu/20260923T011529Z-9e0e98` and
`artifacts/qemu/20260923T011529Z-6920f5`.

On the native NVMe installation, Haiku validated the exact I2S resource and
published the endpoint. `media_client` played a five-second, 1 kHz, 48 kHz
stereo WAV to completion. The driver reported playback start and a clean stop
with zero underruns. The final transcript is
`artifacts/audio/native/haiku-audio-playback-v3.txt`; Linux codec/I2S oracle
captures and the complete serial history are retained in the same artifact
tree.

The first native playback exposed two integration defects which are covered by
the host checks: thread-context FIFO priming now disables interrupts around its
spinlock, and force-stop no longer frees buffers before the media add-on writes
its closing silence. The final playback produced no kernel or media add-on
fault.

## Current scope

Qualification covers the board's ES8316 analog playback path at 48 kHz stereo
S16. The remote fixture proves Media Kit completion, interrupt-driven sample
delivery, codec unmute and zero driver underruns; it does not include an
external analog waveform capture. Microphone input, front-panel routing,
volume controls, other sample formats and rates, DMA, HDMI/DisplayPort audio
and S/PDIF remain separate roadmap work.
