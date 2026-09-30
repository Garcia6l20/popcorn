# A two-channel SPI ADC mocked in Python on SPI1, HAL

An STM32U575 application reads a two-channel 12-bit ADC on SPI1, PA5 SCK, PA6 MISO and PA7
MOSI, with a software chip select on PB0. Each conversion is one chip select period of two
16-bit frames sent with `HAL_SPI_TransmitReceive` in polling mode: a command with the start
bit and the channel, then a frame that brings the sample back. The firmware converts both
channels once per millisecond of SysTick time, 16 times, into `adc_result.samples`, then
sleeps in WFI.

The ADC is not a popcorn device. It is `spiadc.Adc`, a `popcorn.board.SpiTarget` written next
to the test. Its samples come from a Python function of the channel and the virtual time, a
ramp by default.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/spi_mock
```

## Read first

- `spiadc.py`: the whole mock. The SPI model calls `on_select` on each edge of PB0 and
  `on_exchange` for each frame while it is low. The annotated fields are the state a snapshot
  keeps. The source and the ramp slope are read-only: the `machine` fixture reuses one ADC
  for every test of a config.
- `popcorn.toml`: `[models] python = ["."]` puts this directory on `sys.path`, so
  `model = "spiadc:Adc"` resolves. `cs = "PB0"` is the chip select, `counts_per_ms` the ramp
  slope.
- `test_spi_mock.py`:
  - the ramp read back from `adc_result`, rising with virtual time, channel 1 its mirror;
  - one select and one deselect per conversion, each holding two 16-bit frames;
  - `Machine(devices={"SPI1": [Adc(cs="PB0", source=source)]})` built in the test, with a
    Python function as the source that also records what it served. It shows what a project
    file cannot pass. A hand-built machine gets only what the test passes: not the project's
    `[clock]`, `[models] strict`, `[boot]` vtor, arena_base and memory_init, `[board.X] model`,
    `[board.RCC]` or pty bridges. This project file sets none of them.
    `popcorn_project.boot(machine)` runs `[boot] until` within its budget. The plugin's failure
    report does not cover such a machine.
- `firmware/main.c`: the chip select and the two-frame conversion.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/spi_mock/spi_adc.elf examples/stm32u575/spi_mock/prebuilt/
```
