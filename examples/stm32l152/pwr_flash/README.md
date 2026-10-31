# Boot counter in data EEPROM and a flash page program, HAL

An STM32L152 application raises its clock to 32 MHz from HSI and the PLL in voltage range 1
with one flash wait state, RM0038 Table 13. It reads a boot counter from the first word of the
data EEPROM at 0x08080000 and writes it back incremented with `HAL_FLASHEx_DATAEEPROM_Unlock`,
`HAL_FLASHEx_DATAEEPROM_Program` and `HAL_FLASHEx_DATAEEPROM_Lock`. Then it erases the 256-byte
page holding its `data_page` array with `HAL_FLASHEx_Erase` and programs a 4-word record there
with `HAL_FLASH_Program`. An erased STM32L1 flash or EEPROM word reads 0, RM0038 3.4.2. Each test
runs twice:
on the xe build on an STM32L152RETx and on the xb build on the Cat.1 STM32L152RBTx.

It ports `stm32u575/pwr_flash`. The L1 FLASH unlocks through PEKEYR and PRGKEYR in FLASH_PECR,
erases 256-byte pages and programs words.

`MachineDescription(strict=True)` makes any register access the models do not serve, and any
flash wait state count below RM0038 Table 13 for the clock and voltage range, fail the run
instead of being logged.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l152/pwr_flash
```

## Read first

- `test_pwr_flash_l152.py`: the counter read and set through `machine.mem`, the page read erased
  then programmed, PECR read through the peripheral view, and a strict run failing at a store of
  0 wait states.
- `firmware/main.c`: the clock sequence, the counter and the page program.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m3/examples/stm32l152/pwr_flash/pwr_flash*.elf examples/stm32l152/pwr_flash/prebuilt/
```

The xe build links `../STM32L152XE_FLASH.ld`, 256 KB flash and 32 KB SRAM, the xb build
`../STM32L152XB_FLASH.ld`, 128 KB and 16 KB, so each stack lies inside the SRAM of every part it
runs on.
