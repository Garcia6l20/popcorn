# Push button and LED on EXTI, HAL

An STM32L476 application drives an LED on PA5 and takes a push button on PC13, as on a
Nucleo-L476RG. `HAL_GPIO_Init` sets PC13 as an input with a pull-up and an interrupt on the
falling edge: it routes EXTI line 13 to port C in SYSCFG_EXTICR4, RM0351 9.2.6, and enables the
line in EXTI_IMR1 and EXTI_FTSR1, RM0351 14.5. Each press runs the EXTI15_10 handler, which
counts it in `presses` and toggles the LED. The main loop sleeps in WFI. The machine runs with
`strict=True`.

It ports `stm32l152/gpio`. The L4 EXTI takes its port selection from SYSCFG, as the L1 does,
and keeps lines 0 to 31 in its first register bank.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l476/gpio
```

## Read first

- `test_gpio_l476.py`: the host drives PC13 through `machine.periph.GPIOC.pin(13)` and reads
  PA5 back, the EXTI routing read through the SYSCFG and EXTI views, the interrupt entries of the
  trace ring, and a block trace of the idle loop.
- `firmware/main.c`: the pin setup, the EXTI callback and the WFI loop.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m4f/examples/stm32l476/gpio/gpio_button.elf examples/stm32l476/gpio/prebuilt/
```

The build links `../STM32L476XX_FLASH.ld` for 256 KB flash and the 96 KB SRAM1.
