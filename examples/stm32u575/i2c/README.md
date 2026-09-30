# 24xx EEPROM write, acknowledge poll and read back on I2C1, LL

An STM32U575 application drives I2C1 in fast mode, PB6 SCL and PB7 SDA, with a polling LL
driver. TIMINGR 0x00100305 gives 400 kHz by RM0456's SCL formula without the tSYNC delays,
which popcorn does not model. The same TIMINGR runs slower on silicon, about 250 to 300 kHz. It writes a 32-byte page at 0x0100 to a 24xx EEPROM at address 0x50. It then polls the
address until the EEPROM acknowledges again, counting the NACKed polls of the write cycle, and
reads the page back. `eeprom_result` holds what it wrote, what it read and the poll count.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/i2c
```

## Read first

- `popcorn.toml`: the EEPROM on I2C1 as `popcorn.devices:Eeprom24xx` with a 5 ms write
  cycle. `[configs.slow-eeprom]` doubles the write cycle. `[configs.scripted]` replaces the
  EEPROM with a `popcorn.devices:TableTarget` that answers reads of register 0x0100 from a
  table.
- `test_i2c.py`:
  - `Eeprom24xx.contents()` checked against what the firmware wrote.
  - `await machine_factory.booted("slow-eeprom")` builds a second machine from another config
    inside an async test, where the poll count doubles. A sync test calls
    `machine_factory("slow-eeprom")` instead.
  - `@pytest.mark.popcorn(config="scripted")` runs one test on the scripted board, checking
    `TableTarget.writes()` and the bus time at popcorn's 400 kHz.
- `firmware/main.c`: the LL transfers and the acknowledge polling.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/i2c/i2c_eeprom.elf examples/stm32u575/i2c/prebuilt/
```
