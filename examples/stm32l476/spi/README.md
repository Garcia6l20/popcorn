# A three-axis accelerometer mocked in Python on SPI1, HAL

An STM32L476 application reads a three-axis accelerometer on SPI1, PA5 SCK, PA6 MISO and PA7
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

It ports `stm32l152/spi` to the STM32L4 SPI, RM0351 42, with its 32-bit FIFOs and a
programmable frame size: `HAL_SPI_Init` sets DS 0111 for 8-bit frames and FRXTH 1, so RXNE
rises at each received byte, RM0351 42.4.6 and 42.4.9. `MachineDescription(strict=True)` makes
any register access the SPI model does not serve fail the run.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l476/spi
```

## Read first

- `accel.py`: the whole mock. The SPI model calls `on_select` on each edge of PB6 and
  `on_exchange` for each frame while it is low. The annotated fields are the state a snapshot
  keeps.
- `conftest.py`: `Device(Accelerometer, cs="PB6", counts_per_ms=100)` on SPI1, strict.
- `test_spi_l476.py`:
  - the identity and the control register the firmware wrote;
  - the samples against the ramp, Y the negative of X and Z one g;
  - one chip select period per transfer;
  - SPI1EN set by `HAL_SPI_MspInit`, the AF5 pins and CPOL and CPHA;
  - DS and FRXTH for 8-bit frames, and the RX FIFO empty after the last transfer;
  - `machine_factory.booted(spi_description, devices={"SPI1": [Accelerometer(cs="PB6",
    mode=0)]})`, a device in mode 0, which ends the run with `ConfigError`.
- `firmware/main.c`: the chip select and the three transfer shapes.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m4f/examples/stm32l476/spi/spi_accel.elf examples/stm32l476/spi/prebuilt/
```

The build links `../STM32L476XX_FLASH.ld` for 256 KB flash and the 96 KB SRAM1.
