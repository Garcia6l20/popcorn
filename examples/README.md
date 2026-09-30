# popcorn examples

Each directory is a firmware project for one chip with the pytest suite that runs it on popcorn.
Copy one, build it, run its tests, then read its `test_*.py` to see how a test drives the machine.

| example | firmware style | shows |
| --- | --- | --- |
| [stm32u575/gpio](stm32u575/gpio) | HAL | a host-driven button on an EXTI falling edge toggling an LED, WFI between presses |
| [stm32u575/gpdma](stm32u575/gpdma) | HAL DMA | USART1 TX over GPDMA1, a normal block then a two-node linked list, frames back to back while the CPU sleeps in WFI |
| [stm32u575/i2c](stm32u575/i2c) | LL | a 24xx EEPROM page write with acknowledge polling, configs swapping the device, a scripted TableTarget |
| [stm32u575/i2c_mock](stm32u575/i2c_mock) | LL | a temperature sensor written in Python as an `I2CTarget`: scripted readings, the configuration word the firmware wrote, NACK injection and recovery |
| [stm32u575/iwdg](stm32u575/iwdg) | HAL | IWDG refresh, a watchdog reset request 500 ms after the last refresh, IWDGRSTF after the reset |
| [stm32u575/python_model](stm32u575/python_model) | HAL IT | the RNG, which popcorn does not model, served by a `popcorn.model.Model` written in Python: register handlers, a scheduled DRDY, the interrupt line, a seed from the project file |
| [stm32u575/rcc](stm32u575/rcc) | LL | HSE bypass to a 160 MHz PLL1, voltage range, flash latency, SysTick period in virtual time |
| [stm32u575/tim](stm32u575/tim) | LL | TIM2 1 kHz update interrupt toggling PA5, TIM16 one-pulse after 50 ms, update period in virtual time |
| [stm32u575/usart](stm32u575/usart) | HAL IT | a printf banner and a receive-to-idle line echo, frame timing at the BRR rate |
| [stm32u575/uart_mock](stm32u575/uart_mock) | HAL IT | an AT modem written in Python on the host end of USART2: replies in virtual time from the test's coroutine, an ERROR reply, a reply that never comes and the firmware's timeout |
| [stm32u575/spi](stm32u575/spi) | FreeRTOS, HAL init, LL driver | AT25SF NOR flash on the hardware SS output, an interrupt-driven transfer from a task, an application linked behind a bootloader offset |
| [stm32u575/spi_mock](stm32u575/spi_mock) | HAL | a two-channel ADC written in Python as a `SpiTarget` on a software chip select: samples from a function of virtual time, chip select framing, `Machine(devices=)` next to the project file |

Every example has the same layout:

- `firmware/` the C sources, as a CubeMX project would have them
- `prebuilt/<name>.elf` the committed firmware, used as is without arm-none-eabi-gcc
- `popcorn.toml` the project file: chip, image, boot, board devices
- `test_*.py` the tests, and `conftest.py` loading the `popcorn.pytest` plugin
- `pytest.ini` makes the example directory the pytest rootdir, so its `popcorn.toml` is the
  project

Run one example from the repository root:

```sh
uv run pytest -q examples/stm32u575/spi
```

With arm-none-eabi-gcc on PATH, `uv run pcons -B build -j4` rebuilds every example's firmware.
`uv run pcons -B build -j4 firmware-check` then compares each rebuilt ELF with its prebuilt copy,
loadable content only.
