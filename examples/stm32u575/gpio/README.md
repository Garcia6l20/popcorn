# Push button toggles an LED through EXTI, HAL

An STM32U575 application configures PC13 as an input with a pull-up and an EXTI interrupt on
the falling edge, the button pulling the pin to ground, and PA5 as a push-pull output driving
an LED. `HAL_GPIO_EXTI_Falling_Callback` counts the presses and toggles PA5. The main loop sleeps
in WFI, and the HAL tick on SysTick is the only other wake-up.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/gpio
```

## Read first

- `test_gpio.py`: the host drives PC13 through `machine.periph.GPIOC.pin(13).level`, releases it
  back to its pull-up with `release()`, and reads PA5 with `machine.periph.GPIOA.pin(5).level`.
  The trace ring's IRQ records show that only SysTick wakes the firmware in 10 ms, and one
  EXTI13 entry per press. A full block trace, `machine.trace([TraceKind.BLOCK], path)`, shows
  the WFI sleep: a few blocks per wake-up.
- `firmware/main.c`: the pin setup and the callback.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/gpio/gpio_button.elf examples/stm32u575/gpio/prebuilt/
```
