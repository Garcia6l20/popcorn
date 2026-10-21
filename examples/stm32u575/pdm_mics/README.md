# Two PDM microphones on MDF1, one interleaved GPDMA ring, HAL

An STM32U575 application records two PDM microphones with the multi-function digital filter,
MDF1, through ST's HAL:

- The kernel clock is PLL3 Q at 20.48 MHz: HSI16, M 4, N 128, Q 25. SYSCLK is HSI16, so hclk at
  16 MHz stays above mdf_proc_ck. Voltage range 2 allows PLL3, which range 4 does not.
- `HAL_MDF_Init` on filter 0 sets the common parameters: two interleaved filters, ILVNB 1,
  processing clock divider 5 for mdf_proc_ck 4.096 MHz, CCK0 as an output at divider 2,
  2.048 MHz. Each `HAL_MDF_Init` then enables its serial interface in LF_MASTER SPI mode on
  CCK0 and selects its bitstream: filter 0 the falling edge of SITF0, filter 1 the rising edge
  of SITF1.
- Both filters run Sinc5 with decimation 32, gain -12 dB, the reshape filter with decimation 4
  and the high-pass filter at 0.0025 x FPCM: 2.048 MHz / 32 / 4 = 16 kHz per microphone.
  Acquisition is synchronous continuous on TRGO.
- `HAL_MDF_AcqStart` starts filter 1, `HAL_MDF_AcqStart_DMA` starts filter 0 with a circular
  GPDMA1 channel 0 linked list and `MsbOnly`, then `HAL_MDF_GenerateTrgo` starts both on one
  trigger.
- With ILVNB 1, DFLT0DR reads filter 0 and filter 1 in turn, so `ring`, an `int16_t[128]`, holds
  64 sample pairs, filter 0 first. The half and complete callbacks count the ring halves.
- After four complete callbacks the firmware stops both filters with `HAL_MDF_AcqStop_DMA` and
  `HAL_MDF_AcqStop` and reaches `captured`.

The `MachineDescription` in `conftest.py` needs no board entry: every microphone and filter
output of MDF1 is connected when the machine is built. A test feeds a microphone and reads a
filter through `m.periph.MDF1`:

```python
await m.periph.MDF1.mic(0, "falling").write_pcm(samples, hz=16_000)
await m.periph.MDF1.mic(1, "rising").write_pcm(other, hz=16_000)
stop = await m.run(until="captured", budget=9_100, for_=us(21_000))
first = m.periph.MDF1.output(0).take()
```

`write_pcm` queues 16-bit PCM that a sigma-delta modulator turns into the bitstream, one bit per
clock edge of the microphone's kind. `output(n).take()` returns every sample filter n wrote to
its RXFIFO, a signed 24-bit `pcm` with its virtual time `at`.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/pdm_mics
```

## Read first

- `test_pdm_mics.py`: writes a 1 kHz sine to the SITF0 falling-edge microphone and a 3 kHz sine
  to the SITF1 rising-edge one, 16 kHz PCM at -12 dBFS, runs to `captured` and checks:
  - 256 samples per filter, none overrun, and `ring` holds the last 128 words of the two
    outputs interleaved, filter 0 first, each `pcm >> 8`.
  - each filter carries its own tone at -2 to -3.5 dBFS of a 24-bit full scale and the other
    tone below -70 dBFS.
  - four half and four complete callbacks, each the same delay after the 32nd and 64th sample
    pair of its ring pass, samples 62.5 us apart, no error callback.
  - the registers the HAL wrote: PROCDIV 4, CCKDIV 1, SITFMOD 0, BSSEL 1 and 2, ILVNB 1, ACQMOD
    010 and SCALE 0x2C.
  - at -6 dBFS filter 0 reaches -2^23 and `HAL_MDF_ErrorCallback` sees `MDF_ERROR_SATURATION`.
- `firmware/main.c`: the clocks, the two `HAL_MDF_Init` calls, the linked-list node and the start
  order.

Every test runs with `strict=True` in its `popcorn` marker: a register write that breaks an
RM0456 rule raises ModelError instead of logging a warning.

## The clock ratio, RM0456 Table 374

Table 374 asks mdf_proc_ck above twice the CCK rate in LF_MASTER SPI mode and above four times
in normal SPI mode. popcorn reads its ">" as ">=". Here mdf_proc_ck is exactly 2 x CCK0, as
in the RM's own configuration of Table 384, so LF_MASTER is clean. With
`MDF_SITF_NORMAL_SPI_MODE` and the same dividers the model logs a Table 374 warning once at
the SITFEN write in `HAL_MDF_Init`, and raises ModelError naming SITF0CR when the machine is
strict. Normal SPI at 2.048 MHz needs mdf_proc_ck of at least 8.192 MHz, for instance
`ProcClockDivider` 2 and `OutputClock.Divider` 5, which give the same samples.

## The output level

Sinc5 with decimation 32 gives 26 bits, and the -12 dB gain, SCALE 0x2C, brings them to the
24 bits the reshape filter takes. The reshape filter adds about 9.5 dB, so a -12 dBFS input
leaves the chain near -2.5 dBFS, and input above about -9.5 dBFS saturates: SATF rises and
`HAL_MDF_ErrorCallback` runs with `MDF_ERROR_SATURATION`. The reshape and high-pass filter
arithmetic is popcorn's own, designed to the RM's cutoff and gain figures. It is not bit-exact
with silicon.

## What the HAL writes

- `HAL_MDF_Init` writes SITFEN 1 before any filter is configured, so each serial interface
  clocks bits from then on. A filter in a synchronous mode takes its first bit at the first edge
  of its stream after the TRGO write, so filter 1, on the rising edge, starts half a bit period
  before filter 0.
- `HAL_MDF_AcqStart_DMA` with `MsbOnly` points the channel at DFLT0DR + 2 and reads half words:
  the 16 MSBs of the 24-bit sample. Any read of DFLTxDR pops one sample, whatever its size.
- In interleaved mode only filter 0 drives the DMA request: it rises when both RXFIFOs hold a
  sample. Filter 1 needs no DMA and is started with `HAL_MDF_AcqStart`, with the same filter
  configuration as filter 0.
- The stop functions clear the flags with `DFLTxISR |= ...`, a read-modify-write that clears
  every flag set at that time.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j10
cp build/example-firmware/examples/stm32u575/pdm_mics/pdm_mics.elf examples/stm32u575/pdm_mics/prebuilt/
```
