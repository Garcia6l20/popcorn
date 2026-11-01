# A three-axis accelerometer mocked in Python on SPI1, HAL

An STM32L152 application reads a three-axis accelerometer on SPI1, PA5 SCK, PA6 MISO and PA7
MOSI on AF5, with a software chip select on PB6, in SPI mode 3 and 8-bit frames. Each
transfer is one chip select period sent with `HAL_SPI_TransmitReceive` in polling mode: a
command frame with the read and auto-increment bits and the register address, then one frame
per register. The firmware reads WHO_AM_I, writes CTRL_REG1, then reads the six output
registers 8 times into `accel_result.samples`, with `HAL_Delay(1)` between them, one to two
milliseconds of SysTick time apart.

The accelerometer is `accel.Accelerometer`, a `popcorn.board.SpiTarget` written next to the
test, on the common LIS3DH register layout. Its X axis is a ramp in virtual time, Y its
negative, Z one g. It declares SPI mode 3, which the SPI model checks against the firmware's
CPOL and CPHA.

It ports `stm32u575/spi_mock` to the STM32L1 SPI, one transmit and one receive buffer with
TXE and RXNE. `MachineDescription(strict=True)` makes any register access the SPI model does
not serve fail the run.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l152/spi
```

## Read first

- `accel.py`: the whole mock. The SPI model calls `on_select` on each edge of PB6 and
  `on_exchange` for each frame while it is low. The annotated fields are the state a snapshot
  keeps.
- `conftest.py`: `Device(Accelerometer, cs="PB6", counts_per_ms=100)` on SPI1, strict.
- `test_spi_l152.py`:
  - the identity and the control register the firmware wrote;
  - the samples against the ramp, Y the negative of X and Z one g;
  - one chip select period per transfer;
  - SPI1EN set by `HAL_SPI_MspInit`, the AF5 pins and CPOL and CPHA;
  - `machine_factory.booted(spi_description, devices={"SPI1": [Accelerometer(cs="PB6",
    mode=0)]})`, a device in mode 0, which ends the run with `ConfigError`.
- `firmware/main.c`: the chip select and the three transfer shapes.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m3/examples/stm32l152/spi/spi_accel.elf examples/stm32l152/spi/prebuilt/
```

The build links `../STM32L152XE_FLASH.ld`, 256 KB flash and 32 KB SRAM, so the stack lies inside
the SRAM of every part it runs on.
