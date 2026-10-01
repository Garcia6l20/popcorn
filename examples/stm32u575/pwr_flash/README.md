# Voltage scaling, flash wait states, page erase and program, LL

An STM32U575 application raises its clock from the 4 MHz MSIS reset clock to 160 MHz, in the
order RM0456 section 10.5.4 gives: booster prescaler and PLL1 source, regulator range 1, EPOD
booster, 4 flash wait states, then PLL1 from MSIS, M 1, N 80, R 2, as SYSCLK, and the
instruction cache on. RAMCFG WSC stays 0: RM0456 Table 47 asks for no SRAM wait state up to
160 MHz in range 1. Then it programs its own flash with plain register accesses, as RM0456
sections 7.3.6 and 7.3.7 describe: unlock FLASH_NSCR, erase the last page of bank 2 with PER
and STRT, program one quad-word with PG and four word stores, wait for BSY each time. A last
store into flash without PG shows the PGSERR flag. Last, it reads the device electronic
signature of RM0456 chapter 76 with `LL_GetFlashSize`, `LL_GetPackageType` and the
`LL_GetUID_Word` calls. The code uses LL and CMSIS only.

`[models] strict = true` makes any register access the models do not serve, and any flash wait
state count below RM0456 Table 54 for the clock and voltage range, fail the run instead of being
logged.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/pwr_flash
```

## Read first

- `popcorn.toml`: the chip, the image, a boot to `main`, and strict models. No board device:
  flash is part of the chip.
- `test_pwr_flash.py`: the data page read through `machine.mem` before the erase, after it and
  after the program, the FLASH_NSSR flags through the peripheral view, the erase and program
  times in virtual time, a wait-state count too low for 160 MHz failing the run, and the
  device signature as the firmware read it.
- `firmware/main.c`: the clock sequence, then the erase, program and stray store, then the
  signature reads.

popcorn's STM32U5 description holds the signature page at 0x0BFA0400: the flash size and the
package code of the part from its CubeMX data, and a fixed unique ID, so every run reads the
same signature.

The FLASH model holds BSY for the typical times of the STM32U575 datasheet, DS13737 Table 86:
1.5 ms for a page erase, 118 us for a quad-word. The firmware polls BSY, so the virtual time
between its steps shows them.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/pwr_flash/pwr_flash.elf examples/stm32u575/pwr_flash/prebuilt/
```
