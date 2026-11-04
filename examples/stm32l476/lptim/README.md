# LPTIM1 on the LSE, autoreload and compare matches, HAL

An STM32L476 application runs LPTIM1 on the 32.768 kHz LSE with ST's HAL LPTIM driver.
`HAL_LPTIM_MspInit` starts the LSE, selects it as the LPTIM1 kernel clock with RCC_CCIPR.LPTIM1SEL
11, RM0351 6.4.28, sets LPTIM1EN and enables LPTIM1_IRQn. The firmware starts the counter in
continuous PWM mode with `HAL_LPTIM_PWM_Start_IT`, ARR 4095 and CMP 2047: the counter wraps every
4096 LSE cycles, 125 ms, and matches CMP 62.5 ms before each wrap, RM0351 34.4.9, with ARRM and CMPM
in LPTIM_ISR, RM0351 34.7.1. The autoreload callback counts `periods` and toggles PA5, the compare
callback counts `compares`. The main loop sleeps in WFI.

The LPTIM model keeps the counter in virtual time from the LSE rate, so the tests check each
match time against the period. `MachineDescription(strict=True)` makes any register access the
models do not serve fail the run.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l476/lptim
```

## Read first

- `test_lptim_l476.py`:
  - LSERDY, LPTIM1SEL, LPTIM1EN, ARR, CMP and CFGR after the start;
  - intercepts on `HAL_LPTIM_AutoReloadMatchCallback` and `HAL_LPTIM_CompareMatchCallback`,
    the reloads 125 ms apart, the first 4099 LSE cycles after the start with the 3-cycle start
    latency and the ES0250 2.18.3 one-cycle ARRM delay, and each compare 62.5 ms before its
    reload;
  - PA5 read after each period.
- `conftest.py`: the boot stops at `lptim_started`, after the counter starts.
- `firmware/main.c`: the MSP init, the LPTIM setup and the callbacks.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m4f/examples/stm32l476/lptim/lptim_lse.elf examples/stm32l476/lptim/prebuilt/
```

The build links `../STM32L476XX_FLASH.ld` for 256 KB flash and the 96 KB SRAM1.
