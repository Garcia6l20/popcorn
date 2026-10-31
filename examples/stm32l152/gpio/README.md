# Push button and LED on EXTI, HAL

An STM32L152 application drives an LED on PA5 and takes a push button on PC13, as on a
Nucleo-L152RE. `HAL_GPIO_Init` sets PC13 as an input with a pull-up and an interrupt on the
falling edge: it routes EXTI line 13 to port C in SYSCFG_EXTICR4, RM0038 8.5.6, and
enables the line in EXTI_IMR and EXTI_FTSR. Each press runs the EXTI15_10 handler, which counts
it in `presses` and toggles the LED. The main loop sleeps in WFI. Each test runs twice: on the xe
build on an STM32L152RETx and on the xb build on the Cat.1 STM32L152RBTx.

It ports `stm32u575/gpio`. The L1 EXTI takes its port selection from SYSCFG, not from EXTI
itself, and lines 10 to 15 share one interrupt.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l152/gpio
```

## Read first

- `test_gpio_l152.py`: the host drives PC13 through `machine.periph.GPIOC.pin(13)` and reads
  PA5 back, the EXTI routing read through the SYSCFG and EXTI views, the interrupt entries of the
  trace ring, and a block trace of the idle loop.
- `firmware/main.c`: the pin setup, the EXTI callback and the WFI loop.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m3/examples/stm32l152/gpio/gpio_button*.elf examples/stm32l152/gpio/prebuilt/
```

The xe build links `../STM32L152XE_FLASH.ld`, 256 KB flash and 32 KB SRAM, the xb build
`../STM32L152XB_FLASH.ld`, 128 KB and 16 KB, so each stack lies inside the SRAM of every part it
runs on.
