# Voltage scaling, flash wait states, sector erase and program, LL

An STM32H573 application raises its clock from the 32 MHz HSI reset clock to 250 MHz, in the
order RM0481 section 11.4 gives: regulator range VOS0, 5 flash wait states and a programming
delay of 2 (`WRHIGHFREQ`, RM0481 Table 48), then PLL1 from the HSI as SYSCLK, and the
instruction cache on. Then it programs its own flash with plain register accesses, as RM0481
sections 7.3.5 and 7.3.6 describe: unlock FLASH_NSCR, erase the last sector of bank 2 with SER,
SNB, BKSEL and START, program one quad-word with PG and four word stores, wait for BSY each
time. EOP stays set after the erase until `FLASH_NSCCR.CLR_EOP` clears it. A last store into
flash without PG shows the PGSERR flag. Last, it reads the device electronic signature of
RM0481 section 67 with `LL_GetFlashSize`, `LL_GetPackageType` and the `LL_GetUID_Word` calls.
The code uses LL and CMSIS only.

It ports `stm32u575/pwr_flash`. The H5 erases 8 KB sectors with SER and SNB where the U5 erases
pages with PER and PNB, takes the flags back through `FLASH_NSCCR`, sets EOP without an
interrupt enable, and needs 5 wait states at 250 MHz in VOS0.

`MachineDescription(strict=True)` makes any register access the models do not serve, and any
flash wait state count or programming delay below RM0481 Table 48 for the clock and voltage
range, fail the run instead of being logged.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32h573/pwr_flash
```

## Read first

- `conftest.py`: the chip, the image, a boot to `main`, and `strict=True`. No board device:
  flash is part of the chip.
- `test_pwr_flash.py`: the data sector read through `machine.mem` before the erase, after it
  and after the program, the FLASH_NSSR flags through the peripheral view, the EOP flag and its
  clear in FLASH_NSCCR, the erase and program times in virtual time, a wait-state count too low
  for 250 MHz failing the run, and the device signature as the firmware read it.
- `firmware/main.c`: the clock sequence, then the erase, program and stray store, then the
  signature reads.

popcorn's STM32H5 description holds the signature page at 0x08FFF800: the flash size and the
package code of the part from its CubeMX data, and a fixed unique ID, so every run reads the
same signature.

The FLASH model holds BSY for the typical times of the STM32H573 datasheet, DS14121 Table 52:
2 ms for a sector erase, 31 us for a quad-word. The firmware polls BSY, so the virtual time
between its steps shows them.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32h573/pwr_flash/pwr_flash.elf examples/stm32h573/pwr_flash/prebuilt/
```
