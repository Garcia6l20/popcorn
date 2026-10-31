# Independent watchdog refresh and reset, HAL

An STM32L152 application starts the IWDG with `HAL_IWDG_Init` for 500 ms and refreshes it every
100 ms with `HAL_IWDG_Refresh`. The IWDG counts the 37 kHz LSI divided by 32, so the HAL loads a
reload of 578 and the timeout is 499.89 ms. At boot the firmware records whether IWDGRSTF is set
in RCC_CSR and clears the reset flags.

It ports `stm32u575/iwdg`. The L1 IWDG has no window and no early wake-up interrupt, and the LSI
runs at 37 kHz where the U5 one runs at 32 kHz.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l152/iwdg
```

## Read first

- `test_iwdg_l152.py`: the refreshes captured by an intercept, the refresh switched off through
  `machine.var`, the reset request stop with `StopReason.RESET_REQUEST`, the reset applied with
  `ResetKind.WATCHDOG`, and IWDGRSTF read back by the firmware after it.
- `firmware/main.c`: the watchdog setup, the refresh loop and the reset cause.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m3/examples/stm32l152/iwdg/iwdg_refresh.elf examples/stm32l152/iwdg/prebuilt/
```

The build links `../STM32L152XE_FLASH.ld`, 256 KB flash and 32 KB SRAM, so the stack lies inside
the SRAM of every part it runs on.
