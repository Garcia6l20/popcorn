# Independent watchdog early wakeup, reloaded from a busy loop, LL

An STM32U575 application starts the IWDG with LL: prescaler 8, a 2048-count reload, 512 ms
from the 32 kHz LSI, and the early wakeup interrupt 204 counts before the end with EWIE set. It
waits for the update flags to clear, then reloads KR from a busy loop, counting the reloads in
`reloads`. The early wakeup never fires.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/iwdg_ewi
```

## Read first

- `test_iwdg_ewi.py`: `machine.pending_events` before the watchdog starts and after 50 ms of
  reloads, the reload count and EWIF.
- `firmware/main.c`: the IWDG setup and the reload loop.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/iwdg_ewi/iwdg_ewi.elf examples/stm32u575/iwdg_ewi/prebuilt/
```
