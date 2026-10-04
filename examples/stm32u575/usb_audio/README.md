# USB Audio 2.0 headset on a virtual host, TinyUSB

An STM32U575 application runs TinyUSB 0.21.0 on OTG_FS in device mode, slave FIFO mode: a USB
Audio 2.0 headset at full speed. A speaker streaming interface takes 16 kHz mono 16-bit PCM on
isochronous OUT endpoint 0x01, a microphone streaming interface sends the same format on
isochronous IN endpoint 0x81, both wMaxPacketSize 34, one sample of margin over the 32-byte
nominal packet. One internal fixed clock, its frequency and validity read-only, and a feature
unit on each path, master plus one channel, mute and volume read-write. The main loop copies
what the speaker receives to the microphone. Descriptors use TinyUSB's example values: VID
0xCafe, PID 0x4010, strings `TinyUSB` and `TinyUSB Headset`.

```toml
[board.OTG_FS]
host = {}
```

The host's audio driver binds the audio function at configuration and probes it as Linux
snd-usb-audio does. For each streaming interface: GET_RANGE of the clock's sampling frequency,
2 bytes then all of it, SET_INTERFACE 0, GET_CUR of the clock's validity, of its frequency,
and of its validity again, SET_INTERFACE 1, then SET_INTERFACE 0. Then, for each feature unit,
GET_CUR of mute per channel and master, GET_RANGE of volume three times, the resolution check
Linux makes, GET_CUR, SET_CUR one step, GET_CUR, SET_CUR back, and GET_CUR of the channels not
read by it. `await audio.wait_ready(for_=...)` runs until the probe ended.

```python
audio = machine.periph.OTG_FS.host.audio()
await audio.wait_ready(for_=ms(200))
audio.speaker.format, audio.speaker.sample_rates, audio.speaker.rate
await audio.mic.start()                  # clock reads, then SET_INTERFACE 1
await audio.speaker.start()
await audio.speaker.write(samples)       # interleaved ints
frames = await audio.mic.read_frames(160, for_=ms(200))
await audio.speaker.drain()              # every written sample went out in a packet
audio.mic.clear()                        # drop the frames not read yet
audio.speaker.stop()                     # sync: pipe closed now, SET_INTERFACE 0 next run
await audio.set_volume(unit=2, channel=1, volume=-10 * 256)
```

Each 1 ms frame carries 16 samples, the rate's share of a millisecond. The host's frames are
the device's clock: drift between host and device, which a real headset corrects with feedback
or adaptive clocking, never happens here.

TinyUSB 0.21.0 arms an isochronous OUT endpoint for the frame after the one in which it
handles SET_INTERFACE, and does not handle an incomplete isochronous OUT. The virtual host
retries a NAKed status stage within the frame, so SET_INTERFACE ends in the frame of its SETUP.
Like Linux on an xHCI controller, the host then sends the first packet of the new stream two
frames later, in the other parity: the model loses the first speaker packet after each start.
A real host loses 0 or 1 packet, depending on its isochronous scheduling threshold and on when
it submits: one more frame of delay lands in the armed parity. Write PCM after the first
frames: the tests run 4 ms after `start` before they write.

TinyUSB's microphone flow control sends 15 samples instead of 16 while its FIFO is below half
full, at most once every 11 frames, so the loopback latency grows by a sample at a time until
the FIFO sits at half its depth. Samples come back in order, none lost.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/usb_audio
```

## Read first

- `test_usb_audio.py`: the probe sequence compared with snd-usb-audio's, a sine looped from the
  speaker to the microphone, silence, `drain`, `stop` from sync code with the machine stopped,
  `clear`, and a snapshot taken mid-stream.
- `firmware/usb_descriptors.c`: the device, configuration and string descriptors, built from
  TinyUSB's UAC2 descriptor macros.
- `firmware/main.c`: the clock and USB power setup, the clock and feature unit requests, the
  loopback.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH, which also fetches TinyUSB:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/usb_audio/usb_audio.elf examples/stm32u575/usb_audio/prebuilt/
```
