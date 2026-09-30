# RCC clock tree to 160 MHz, LL

An STM32U575 application switches from the 4 MHz MSIS reset clock to PLL1 at 160 MHz. PLL1 is
fed by a 32 MHz HSE in bypass mode, the usual setup for a board with an external oscillator:
M 3, N 30, R 2. It follows RM0456 section 10.5.4: HSE ready, PLL1 source and booster
prescaler set, regulator range 1, EPOD booster on, 4 flash wait states, then PLL1 enabled. Then SysTick counts milliseconds. The code uses LL only, and
`HSE_VALUE` is 32 MHz so that `SystemCoreClockUpdate` computes the right rate.

`[models] strict = true` makes any register access the RCC, PWR or FLASH models do not serve
fail the run instead of being logged.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/rcc
```

## Read first

- `popcorn.toml`: the chip, the image, a boot to `main`, `[board.RCC] hse_hz` giving the RCC
  model the board's 32 MHz HSE, and strict models.
- `test_rcc.py`: `SystemCoreClock` and `RCC_CFGR1.SWS` read through the symbol and peripheral
  views, the ready flags captured by intercepts, and the SysTick period in virtual time.
  `SystemCoreClock` is what the firmware computes from the PLL1 registers and `HSE_VALUE`. The
  SysTick period checks the real frequency: the core clock follows HCLK, so the period depends
  on the HSE rate the board gives the RCC model.
- `firmware/main.c`: the clock sequence.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/rcc/rcc_clock.elf examples/stm32u575/rcc/prebuilt/
```
