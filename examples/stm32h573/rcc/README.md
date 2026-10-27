# RCC clock tree to 250 MHz, LL

An STM32H573 application switches from the 32 MHz HSI reset clock to PLL1 at 250 MHz. It
follows RM0481 section 11.4: regulator range VOS0, 5 flash wait states, HSI ready, then PLL1
from HSI with M 2, N 31 and a fractional part of 2048/8192, so the VCO runs at 500 MHz and P 2
gives SYSCLK. SYSCLK switches with HCLK divided by 2 first, then HCLK returns to SYSCLK, as the
HAL does. Then SysTick counts milliseconds. The code uses LL only.

It ports `stm32u575/rcc`. The H5 has no EPOD booster: VOS0 takes its place, the HSI is divided
by 2 at reset, and PLL1 takes a fractional N.

`MachineDescription(strict=True)` makes any register access the RCC, PWR or FLASH models do not
serve fail the run instead of being logged.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32h573/rcc
```

## Read first

- `conftest.py`: the chip, the image, a boot to `main` and `strict=True`. The HSI needs no
  board clock.
- `test_rcc.py`: `SystemCoreClock` and `RCC_CFGR1.SWS` read through the symbol and peripheral
  views, the VOS0 and wait-state registers, the ready flags captured by intercepts, and the
  SysTick period in virtual time. `SystemCoreClock` is what the firmware computes from the PLL1
  registers. The SysTick period checks the real frequency: the core clock follows HCLK, so the
  period depends on the SYSCLK the RCC model derives from the PLL1 registers.
- `firmware/main.c`: the clock sequence.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32h573/rcc/rcc_clock.elf examples/stm32h573/rcc/prebuilt/
```
