# Examples

Each example is a firmware project for the STM32U575 or the STM32H573 with the pytest suite that runs it on popcorn. Copy one, build it, run its tests, then read its `test_*.py` to see how a test drives the machine.

Every example has the same layout:

- `firmware/` holds the C sources, as a CubeMX project would have them.
- `prebuilt/<name>.elf` is the committed firmware, used as is without arm-none-eabi-gcc.
- `conftest.py` declares the machine fixtures: chip, image, boot and board devices.
- `test_*.py` are the tests.

Run one from a repository checkout:

```sh
uv run pytest -q examples/stm32u575/gpio
```

## Clocks and power

### rcc

RCC clock tree to 160 MHz, LL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/rcc).

--8<-- "stm32u575/rcc/README.md:3:8"

### pwr_flash

Voltage scaling, flash wait states, page erase and program, LL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/pwr_flash).

--8<-- "stm32u575/pwr_flash/README.md:3:12"

### iwdg

Independent watchdog refresh and reset, HAL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/iwdg).

--8<-- "stm32u575/iwdg/README.md:3:6"

### iwdg_ewi

Independent watchdog early wakeup, reloaded from a busy loop, LL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/iwdg_ewi).

--8<-- "stm32u575/iwdg_ewi/README.md:3:6"

## GPIO and timers

### gpio

Push button toggles an LED through EXTI, HAL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/gpio).

--8<-- "stm32u575/gpio/README.md:3:6"

### tim

Periodic timer interrupt and a one-pulse timer, LL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/tim).

--8<-- "stm32u575/tim/README.md:3:6"

## Analog

### adc

ADC1 internal channels, LL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/adc).

--8<-- "stm32u575/adc/README.md:3:8"

### adc_watchdog

A TIM15-triggered ADC1 scan with GPDMA and an analog watchdog, HAL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/adc_watchdog).

--8<-- "stm32u575/adc_watchdog/README.md:3:13"

## Buses

### i2c

24xx EEPROM write, acknowledge poll and read back on I2C1, LL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/i2c).

--8<-- "stm32u575/i2c/README.md:3:7"

### i2c_mock

A temperature sensor mocked in Python on I2C1, LL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/i2c_mock).

--8<-- "stm32u575/i2c_mock/README.md:3:8"

### spi

SPI NOR flash from FreeRTOS, behind a bootloader offset. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/spi).

--8<-- "stm32u575/spi/README.md:3:7"

### spi_mock

A two-channel SPI ADC mocked in Python on SPI1, HAL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/spi_mock).

--8<-- "stm32u575/spi_mock/README.md:3:8"

## Serial and DMA

### usart

printf banner and line echo on USART1, HAL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/usart).

--8<-- "stm32u575/usart/README.md:3:6"

### uart_mock

An AT modem mocked in Python on the host end of USART2, HAL IT. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/uart_mock).

--8<-- "stm32u575/uart_mock/README.md:3:7"

### gpdma

USART TX over GPDMA, HAL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/gpdma).

--8<-- "stm32u575/gpdma/README.md:3:9"

### linked_uart

One controller and two responders linked by UART, HAL DMA. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/linked_uart).

--8<-- "stm32u575/linked_uart/README.md:3:10"

## USB

### otg_fs

USB OTG_FS core bring-up, HAL PCD. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/otg_fs).

--8<-- "stm32u575/otg_fs/README.md:3:5"

### usb_cdc

USB device enumerated by a virtual host, TinyUSB. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/usb_cdc).

--8<-- "stm32u575/usb_cdc/README.md:3:11"

### usb_audio

USB Audio 2.0 headset on a virtual host, TinyUSB. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/usb_audio).

--8<-- "stm32u575/usb_audio/README.md:3:10"

## Audio

### sai

A sine table played on SAI1 block A by circular GPDMA, HAL DMA. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/sai).

--8<-- "stm32u575/sai/README.md:3:9"

### sai_codec

An I2C codec clocking a slave SAI1 pair, 2D circular GPDMA, HAL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/sai_codec).

--8<-- "stm32u575/sai_codec/README.md:3:16"

### pdm_mics

Two PDM microphones on MDF1, one interleaved GPDMA ring, HAL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/pdm_mics).

--8<-- "stm32u575/pdm_mics/README.md:3:22"

## Random numbers and CRC

### crc

CRC calculation unit, HAL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/crc).

--8<-- "stm32u575/crc/README.md:3:11"

### rng

A 64-byte key from the built-in RNG, LL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/rng).

--8<-- "stm32u575/rng/README.md:3:15"

## Models and plugins

### python_model

A built-in model replaced by a Python model: the RNG, HAL IT. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/python_model).

--8<-- "stm32u575/python_model/README.md:3:6"

### native_plugin

A C++ model and a C++ device from a native plugin: the RNG and an SPI ADC, HAL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/native_plugin).

--8<-- "stm32u575/native_plugin/README.md:3:6"

## TrustZone

### trustzone

Secure and Non-secure pair with TrustZone, CMSE. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32u575/trustzone).

--8<-- "stm32u575/trustzone/README.md:3:6"

## STM32H573

The same layout on the STM32H573. Each example ports its STM32U575 namesake and runs the same way, from `examples/stm32h573/<name>`.

### h573 rcc

HSI to a 250 MHz PLL1 with a fractional N, VOS0 and 5 wait states, LL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32h573/rcc).

--8<-- "stm32h573/rcc/README.md:3:10"

### h573 pwr_flash

Voltage scaling, flash wait states, sector erase and program, LL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32h573/pwr_flash).

--8<-- "stm32h573/pwr_flash/README.md:3:15"

### h573 usart

printf banner and line echo on UART9, HAL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32h573/usart).

--8<-- "stm32h573/usart/README.md:3:10"

### h573 spi

SPI NOR flash on SPI4 from FreeRTOS and GPDMA, behind a bootloader offset. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32h573/spi).

--8<-- "stm32h573/spi/README.md:3:12"

### h573 gpdma

UART9 TX over GPDMA, HAL. [Sources and tests](https://github.com/Garcia6l20/popcorn/tree/main/examples/stm32h573/gpdma).

--8<-- "stm32h573/gpdma/README.md:3:12"
