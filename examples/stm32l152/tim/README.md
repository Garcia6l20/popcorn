# Timer update, one-pulse and input capture, HAL

An STM32L152 application switches the core to the 16 MHz HSI in voltage range 1 with one wait
state, then starts three timers with ST's HAL TIM driver. TIM2 counts at 1 MHz and its update
interrupt at 1 kHz increments `ticks` and toggles PA5. TIM6 counts at 1 kHz in one-pulse mode
and its single update after 50 ms records the tick count it fired at. TIM3 channel 1 captures
the rising edges of PA6 on AF2 at 1 MHz, and the capture callback keeps the count between the
last two edges in `capture_period`. The main loop sleeps in WFI.

It ports `stm32u575/tim`, with TIM6 for the one-pulse timer and an input capture added. The
host plays the capture input: it drives PA6 through the GPIO pin view at chosen virtual times.
`MachineDescription(strict=True)` makes any register access the models do not serve fail the
run.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l152/tim
```

## Read first

- `test_tim_l152.py`:
  - the HSI clock, the TIM enables and the prescalers HAL computed from `SystemCoreClock`;
  - an intercept on `TIM2_IRQHandler` timing each update 1 ms after the last, PA5 toggling;
  - the TIM6 one-pulse firing once, its CEN cleared after the update;
  - PA6 driven high and low at a chosen period, the captured period in timer counts, and
    `TIM3.CNT.read()`, a bus read, against the last captured count.
- `conftest.py`: the boot stops at `timers_started`, after the three timers start.
- `firmware/main.c`: the clock setup, the timer setups and the HAL callbacks.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m3/examples/stm32l152/tim/tim_ticks.elf examples/stm32l152/tim/prebuilt/
```

The build links `../STM32L152XE_FLASH.ld`, 256 KB flash and 32 KB SRAM, so the stack lies inside
the SRAM of every part it runs on.
