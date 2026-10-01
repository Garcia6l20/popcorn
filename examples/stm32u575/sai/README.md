# A sine table played on SAI1 block A by circular GPDMA, HAL DMA

An STM32U575 application plays a 1 kHz sine on SAI1 block A, PE4 FS, PE5 SCK and PE6 SD:
I2S, 16-bit stereo at 48 kHz, the right channel the negated left one. The block is the I2S
master. Its kernel clock is PLL2P, MSIS 4 MHz x 96 / 25 = 15.36 MHz, and without the master
clock divider a 32-bit frame takes 10 x 32 kernel cycles, exactly 48 kHz. GPDMA1 channel 0
feeds the table, 48 frames, from a one-node circular linked list. `HAL_SAI_Transmit_DMA`
starts it, and the half and complete callbacks count buffers in `playback`. The main loop
sleeps in WFI.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/sai
```

## Read first

- `test_sai.py`: `machine.periph.SAI1.audio("A")` is the far end of the block. It holds every
  frame since power-on. `read_frames` returns each frame's slot words, layout and start
  time. The tests check:
  - 480 frames equal the table, read back through `machine.var("sine")`, ten times over.
    `dropped == 0` guards the stream's bound: it keeps 262 144 frames, 5.4 s at 48 kHz, and
    drops the oldest beyond, far from 480 frames but reached by a long run read too late;
  - frames one frame time apart, to the picosecond, the frame time derived from the PLL2 and
    SAI registers;
  - the half and complete callback counts against the frames sent, within the SAI FIFO.
- `firmware/main.c`: the PLL2 clock for SAI1, the SAI init, the circular DMA queue and the
  callbacks.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/sai/sai_sine.elf examples/stm32u575/sai/prebuilt/
```
