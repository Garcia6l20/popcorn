# RCC clock tree to 80 MHz, HAL

An STM32L476 application raises SYSCLK from the 4 MHz MSI reset clock to the main PLL at 80 MHz.
It follows RM0351 sections 5.1.8 and 6.2: voltage range 1, the PWR_CR1 reset range, which
`HAL_PWREx_ControlVoltageScaling` keeps, then the PLL from MSI range 6 with PLLM 1,
PLLN 40 and PLLR 2, so the VCO runs at 160 MHz. `HAL_RCC_ClockConfig` sets the four wait states
of RM0351 Table 11 for 80 MHz in range 1 before it switches SYSCLK to the PLL, then restarts
SysTick at 1 ms. `HAL_RCC_MCOConfig` routes SYSCLK divided by 16 to the MCO pin PA8, alternate
function 0.

MCOSEL and MCOPRE are storage in the RCC model, so the tests read the MCO selection and the PA8
pin mode.

`MachineDescription(strict=True)` makes any register access the RCC, PWR, FLASH or GPIO models
do not serve fail the run instead of being logged.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l476/rcc
```

## Read first

- `conftest.py`: the STM32L476RCTx, a boot to `main` and `strict=True`.
- `test_rcc_l476.py`: `SystemCoreClock` and the RCC_PLLCFGR fields read through the symbol and
  peripheral views, the range 1 and PLL ready state captured by intercepts, the four wait
  states, the MCO selection on PA8, and the SysTick period in virtual time.
- `firmware/main.c`: the clock sequence.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m4f/examples/stm32l476/rcc/rcc_clock.elf examples/stm32l476/rcc/prebuilt/
```

The build links `../STM32L476XX_FLASH.ld` for 256 KB flash and the 96 KB SRAM1.
