# Independent watchdog refresh and reset, HAL

An STM32U575 application records at boot whether the independent watchdog caused the last
reset, reading IWDGRSTF in RCC_CSR, then clears the reset flags. It starts the IWDG from the
32 kHz LSI with prescaler 32 and a 500 ms reload, and refreshes it every 100 ms while
`refresh_enabled` is set.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/iwdg
```

## Read first

- `test_iwdg.py`:
  - Refreshes are timed with an intercept on `HAL_IWDG_Refresh`.
  - The host stops the refreshes by writing `refresh_enabled` through `machine.var`.
  - The watchdog reset request stops the run with `StopReason.RESET_REQUEST`, 500 ms after the
    last refresh. `stop.detail` names the requester, here `watchdog`. A window watchdog or
    a SYSRESETREQ reports its own, so `ResetKind[stop.detail.upper()]` is the kind to reset
    with.
  - `machine.reset(kind)` reboots the chip the way that reset does, and the firmware reports
    IWDGRSTF.
  - Without it, the next `run` or `step` applies the reset the stop reported first, as the
    chip would.
- `firmware/main.c`: the reset cause, the IWDG setup and the refresh loop.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/iwdg/iwdg_refresh.elf examples/stm32u575/iwdg/prebuilt/
```
