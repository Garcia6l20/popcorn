# A temperature sensor mocked in Python on I2C1, LL

An STM32U575 application reads a temperature sensor at address 0x48 on I2C1, PB6 SCL and PB7
SDA, with a polling LL driver. It writes the sensor's configuration register once, then every
10 ms of TIM6 time reads the temperature register: a pointer write, a repeated START and a
2-byte read. It keeps a running mean in `sensor_result`, counts failed transfers in
`sensor_result.errors`, and drives PA5 high while the mean is above 30 degrees. The main loop
sleeps in WFI between samples.

The sensor is not a popcorn device. It is `tempsensor.TempSensor`, a `popcorn.board.I2CTarget`
written next to the test: a pointer register, a 16-bit configuration register, and a
temperature register answering a scripted list of readings.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/i2c_mock
```

## Read first

- `tempsensor.py`: the whole mock. `on_start`, `on_write` and `on_read` are called by the I2C
  model one bus phase at a time. The annotated fields are the state a snapshot keeps, and
  `reset` clears the registers on a power-on. `nack_starts` makes the sensor NACK its address,
  for fault injection. `readings` is read-only: the `machine` fixture reuses one sensor for
  every test of a config, so a test wanting other readings uses a config of `with_configs` or
  builds its own machine with `Machine(devices={"I2C1": [TempSensor(0x48, readings=...)]})`.
  The module is named after the device, not a generic `mocks.py`, so no two examples' mocks
  share a module name on `sys.path`.
- `conftest.py`: `Device(TempSensor, address=0x48, readings=COLD)` on I2C1, `readings` passed to
  the constructor. `with_configs(hot=...)` gives the sensor readings above 30 degrees.
- `test_i2c_mock.py`:
  - `machine.board.devices("I2C1")` returns the mock the project built.
  - an intercept on `sample_taken` counts the samples of runs of fixed virtual length.
  - the mean read through `machine.var("sensor_result")` against the Python mean;
  - the configuration word the firmware wrote, read from the mock;
  - three NACKed addresses: the firmware counts them and goes on sampling;
  - `machine.snapshot()` and `machine.restore()` bring the mock's fields back with the MCU.
- `firmware/main.c`: the LL transfers, the NACK recovery and the TIM6 sample tick.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/i2c_mock/i2c_sensor.elf examples/stm32u575/i2c_mock/prebuilt/
```
