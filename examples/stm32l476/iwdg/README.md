# Independent watchdog refresh, window and reset, HAL

An STM32L476 application starts the IWDG with `HAL_IWDG_Init` for 500 ms and refreshes it every
100 ms with `HAL_IWDG_Refresh`. The IWDG counts the 32 kHz LSI divided by 32, so the HAL loads a
reload of 500 and the timeout is 500 ms. The window comes from `iwdg_window`, 0xFFF by default,
which leaves it open. A refresh while the counter is above IWDG_WINR is a reset, RM0351 36.3.2.
At boot the firmware records whether IWDGRSTF is set in RCC_CSR and clears the reset flags.

It ports `stm32l152/iwdg` and adds the IWDG_WINR window of the L4 IWDG. The firmware waits
100 ms before each refresh, so the first refresh comes 100 ms after `HAL_IWDG_Init`, with the
counter at 400. `MachineDescription(strict=True)` makes any register access the models do not
serve fail the run instead of being logged.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l476/iwdg
```

## Read first

- `test_iwdg_l476.py`: the refreshes captured by an intercept, the refresh switched off through
  `machine.var`, the reset request stop with `StopReason.RESET_REQUEST`, the reset applied with
  `ResetKind.WATCHDOG`, IWDGRSTF read back by the firmware after it, and `iwdg_window` set to
  450, where every refresh lands inside the window, and to 300, where the first refresh resets.
- `firmware/main.c`: the watchdog setup, the refresh loop and the reset cause.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m4f/examples/stm32l476/iwdg/iwdg_refresh.elf examples/stm32l476/iwdg/prebuilt/
```

The build links `../STM32L476XX_FLASH.ld` for 256 KB flash and the 96 KB SRAM1.
