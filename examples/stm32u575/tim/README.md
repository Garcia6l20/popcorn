# Periodic timer interrupt and a one-pulse timer, LL

An STM32U575 application runs TIM2 at 1 kHz from the 4 MHz reset clock: prescaled to 1 MHz,
auto-reload 999. Each update interrupt increments `ticks` and toggles PA5. TIM16 runs in
one-pulse mode, prescaled to 1 kHz with auto-reload 49: its single update interrupt, 50 ms
after start, records `ticks_at_pulse` and counts `pulses`. The main loop sleeps in WFI.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/tim
```

## Read first

- `test_tim.py`: an intercept on `TIM2_IRQHandler` records the virtual time of each update and
  the PA5 level through `machine.periph.GPIOA.pin(5)`. It checks updates exactly 1 ms apart and
  the level alternating. `machine.var` reads the counters after runs of fixed virtual length.
- `firmware/main.c`: the two timer setups and their handlers.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/tim/tim_ticks.elf examples/stm32u575/tim/prebuilt/
```
