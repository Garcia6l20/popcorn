# A temperature sensor mocked in Python on I2C1, HAL

An STM32L152 application reads a temperature sensor at address 0x48 on I2C1, PB8 SCL and PB9
SDA on AF4, at 100 kHz with ST's HAL polling driver. It writes the sensor's configuration
register once with `HAL_I2C_Mem_Write`, then every 10 ms of SysTick time reads the temperature
register with `HAL_I2C_Mem_Read`: a pointer write, a repeated START and a 2-byte read. It keeps
a running mean in `sensor_result`, counts failed transfers in `sensor_result.errors` with the
last HAL error code, and drives PA5 high while the mean is above 30 degrees. The main loop
sleeps in WFI between samples.

The sensor is `thermometer.Thermometer`, a `popcorn.board.I2CTarget` written next to the test:
a pointer register, a 16-bit configuration register, and a temperature register answering a
scripted list of readings. It ports `stm32u575/i2c_mock` to the STM32L1 I2C and its HAL event
sequence, ADDR, BTF and the POS bit of a 2-byte read.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l152/i2c
```

## Read first

- `thermometer.py`: the whole mock. `on_start`, `on_write` and `on_read` are called by the I2C
  model one bus phase at a time. The annotated fields are the state a snapshot keeps, and
  `reset` clears the registers on a power-on. `nack_starts` makes the sensor NACK its address.
- `conftest.py`: `Device(Thermometer, address=0x48, readings=COLD)` on I2C1, strict, and the
  `hot` config with readings above 30 degrees.
- `test_i2c_l152.py`:
  - the mean read through `machine.var("sensor_result")` against the Python mean;
  - the configuration word the firmware wrote, read from the mock;
  - I2C1EN set by `HAL_I2C_MspInit` and PB8 and PB9 open drain on AF4;
  - three NACKed addresses: the HAL returns HAL_I2C_ERROR_AF, the firmware counts them and
    goes on sampling;
  - `machine.snapshot()` and `machine.restore()` bring the mock's fields back with the MCU;
  - below-zero readings through `machine_factory.booted(description, devices=...)`.
- `firmware/main.c`: the HAL transfers, the error count and the 10 ms sample loop.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m3/examples/stm32l152/i2c/i2c_sensor.elf examples/stm32l152/i2c/prebuilt/
```

The build links `../STM32L152XE_FLASH.ld`, 256 KB flash and 32 KB SRAM, so the stack lies inside
the SRAM of every part it runs on.
