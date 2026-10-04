# An I2C codec clocking a slave SAI1 pair, 2D circular GPDMA, HAL

An STM32U575 application streams stereo audio through SAI1 with an external codec as the clock
master:

- Block A is a slave transmitter, block B a slave receiver synchronous to A. Both run I2S,
  16-bit stereo, 32-bit frames.
- The codec is `stereocodec.StereoCodec`, written in Python, on I2C1 at 0x1A. Writing 1 to its
  power register 0x02 starts a 48 kHz frame clock into block A, writing 0 stops it.
- GPDMA1 channel 12 reads `playback`, an `int16_t[2][64]` with one row per channel, and writes
  frames to block A. Channel 13 splits each frame block B receives into the two rows of
  `capture`. Each channel runs one 2D node built with `HAL_DMAEx_List_BuildNode` and
  `DMA_GPDMA_2D_NODE`, in a circular queue.
- Both channels raise their half and complete events per 2D block, 32 and 64 frames.
- The firmware powers the codec on with `HAL_I2C_Mem_Write`, waits for four complete callbacks on
  the receive side, then powers it off and reaches `codec_stopped`.

The project file attaches the codec and wires its clock to block A:

```toml
[models]
python = ["."]

[board.I2C1]
devices = [{ address = 0x1A, model = "stereocodec:StereoCodec" }]

[board.SAI1.A]
clocked_by = "I2C1:0x1A"
```

The codec starts and stops the clock from its I2C write handler:

```python
self.board.set_audio_clock("SAI1", "A", 48_000 if self.power else 0)
```

`clocked_by` is what allows the call: a device may drive the clock of the SAI blocks wired to
it, and the call raises ConfigError for any other block. A test drives the same clock without a
device through `machine.periph.SAI1.audio("A").external_frame_hz`. The Python equivalent of the
project keys is `Machine(devices={"I2C1": [codec]}, audio={"SAI1": {"A":
AudioOptions(clocked_by="I2C1:0x1A")}})`.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/sai_codec
```

## Read first

- `test_sai_codec.py`: queues the receive words, runs to `codec_stopped` and checks:
  - the first frame starts at the codec's power-on byte, frames 1/48 kHz apart to the
    picosecond, the frame in progress at the power-off byte ends and nothing follows.
  - every frame block A sent is `(playback[0][k], playback[1][k])`, k counting modulo 64.
  - `capture[0][k]` and `capture[1][k]` hold the two slots of the last frame k received.
  - four half and four complete callbacks per channel, 64 frames apart.
  - a snapshot mid-stream restored in place replays the same frames, capture and codec stop.
- `stereocodec.py`: the codec and its snapshot state.
- `firmware/main.c`: the two slave blocks, the two 2D nodes and the start order.

## The 2D addressing

A block is one frame: two single-halfword bursts, BNDT 4 bytes. The node repeats it 64 times,
BRC 63. For the transmit side, with `playback` at B:

- SAO = 126, the distance from the end of `playback[0][k]` to `playback[1][k]`.
- BRSAO = 254 with BRSDEC, the step back from the end of `playback[1][k]` to
  `playback[0][k + 1]`.

| burst | reads | SAR after |
| --- | --- | --- |
| 0 of block k | B + 2k, `playback[0][k]` | B + 2k + 2 + 126 = B + 128 + 2k |
| 1 of block k | B + 128 + 2k, `playback[1][k]` | B + 128 + 2k + 2 + 126 - 254 = B + 2k + 2 |

At a block end both the burst offset and the block offset apply, so the second row takes SAO
then BRSAO. After block 63 the node reloads SAR = B. The receive side does the same on the
destination with DAO 126 and BRDAO 254 with BRDDEC. In `HAL_DMAEx_List_BuildNode`, a negative
offset in `RepeatBlockConfig` sets the matching DEC bit:

```c
.RepeatCount      = FRAMES,
.SrcAddrOffset    = CHANNEL_STRIDE - 2,
.BlkSrcAddrOffset = -(2 * CHANNEL_STRIDE - 2),
```

The transfer event mode is `DMA_TCEM_REPEATED_BLOCK_TRANSFER`. On channels 12 to 15 it raises
the half transfer after the rounded-up half of BRC + 1 blocks and the complete at the end of the
2D block. The receive callbacks run 12.0 to 12.17 us after the 32nd frame ends and 15.75 to
15.92 us after the 64th, the time `HAL_DMA_IRQHandler` takes at 4 MHz.

## Starting the streams without HAL_SAI_Transmit_DMA

`HAL_SAI_Transmit_DMA` and `HAL_SAI_Receive_DMA` write the 1D block size into the head node's
CBR1, which clears BRC and the DEC bits of a 2D node. The firmware starts each channel with
`HAL_DMAEx_List_Start_IT`, sets DMAEN, waits for block A's FIFO to fill and then enables block B
before block A.

## The external clock

- Block A has no clock until the codec's power-on byte. Its enable logs
  `slave block without an external frame rate: it sees no frame` and the block waits.
- A clock started from stopped frames from the call. A clock stopped ends the frame in
  progress. A rate change takes effect at the next frame boundary.
- The clock rate and phase are board state. A snapshot keeps them, and a restore resets the
  stream: it drops the frames not read yet and the words not served yet, so the test writes the
  unserved receive words again.
- A power-on or brown-out reset returns the clock to the project wiring, stopped here. Other
  resets keep it running in phase.

`HAL_I2C_Init` sets OA1EN with own address 0, so the I2C model logs that target mode is not
modelled. The controller transfers are unaffected.

## Model limits

- A destination burst left partial by a block end gets no DAO and no warning.
- AFSDET and LFSDET never rise: an external clock always matches the block's frame. A codec with
  another frame length is not modelled.
- FSOFF and FBOFF do not move the frame and slot timing.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/sai_codec/sai_codec.elf examples/stm32u575/sai_codec/prebuilt/
```
