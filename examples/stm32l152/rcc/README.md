# RCC clock tree to 32 MHz, HAL

An STM32L152 application switches from the 2.097 MHz MSI reset clock to the PLL at 32 MHz. It
follows RM0038 sections 5.1.5 and 6.2: voltage range 1 with VOSF polled, the 8 MHz HSE in bypass,
as the ST-LINK MCO feeds it on a Nucleo-L152RE, then the PLL from HSE with PLLMUL 12 and PLLDIV 3,
so the VCO runs at 96 MHz. `HAL_RCC_ClockConfig` sets 64-bit access and the one wait state of
RM0038 Table 13 before it switches SYSCLK, then restarts SysTick at 1 ms.

The L1 HAL sets and clears the RCC_CR enable bits through their bit-band aliases, such as
`RCC_CR_PLLON_BB`, so the run goes through the peripheral bit-band region. Each test runs
twice: on the xe build, `STM32L152xE`, on an STM32L152RETx, and on the xb build, `STM32L152xB`,
on the Cat.1 STM32L152RBTx.

`MachineDescription(strict=True)` makes any register access the RCC, PWR or FLASH models do not
serve fail the run instead of being logged.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l152/rcc
```

## Read first

- `conftest.py`: one description per build, the board HSE at 8 MHz with `RccOptions`, a boot
  to `main` and `strict=True`. `@popcorn.pytest.machine(params=...)` runs each test on both.
- `test_rcc_l152.py`: `SystemCoreClock` and the RCC_CFGR PLL fields read through the symbol and
  peripheral views, the range 1 and wait-state registers, the ready flags captured by
  intercepts, and the SysTick period in virtual time.
- `firmware/main.c`: the clock sequence.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m3/examples/stm32l152/rcc/rcc_clock*.elf examples/stm32l152/rcc/prebuilt/
```

The two builds link `../STM32L152XE_FLASH.ld` and `../STM32L152XB_FLASH.ld`, laid out as ST's
CubeIDE scripts. The xe script gives 256 KB flash and 32 KB SRAM, the xb script 128 KB and
16 KB, so each ELF's stack lies inside the SRAM of every part it runs on.
