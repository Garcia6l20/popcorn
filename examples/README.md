# popcorn examples

Each directory is a firmware project for one chip with the pytest suite that runs it on popcorn.
Copy one, build it, run its tests, then read its `test_*.py` to see how a test drives the machine.

| example | firmware style | shows |
| --- | --- | --- |
| [stm32u575/adc](stm32u575/adc) | LL | ADC1 power-up, calibration, VREFINT and the temperature sensor read against the factory words, timed conversions |
| [stm32u575/adc_watchdog](stm32u575/adc_watchdog) | HAL DMA | a TIM15-triggered 4-channel ADC1 scan, 8x oversampling, circular GPDMA, AWD3 on one input: inputs driven with `ADC1.input(n).volts`, `SupplyOptions(vdda_mv=3300)`, the strict threshold comparison, scan timing |
| [stm32u575/crc](stm32u575/crc) | HAL | the CRC unit from `HAL_CRC_Calculate` and `HAL_CRC_Accumulate`: CRC-32/MPEG-2 on words, CRC-32/ISO-HDLC with input and output reversal, CRC-16/IBM-3740, checked against a bit-serial Python reference |
| [stm32u575/gpdma](stm32u575/gpdma) | HAL DMA | USART1 TX over GPDMA1, a normal block then a two-node linked list, frames back to back while the CPU sleeps in WFI |
| [stm32u575/gpio](stm32u575/gpio) | HAL | a host-driven button on an EXTI falling edge toggling an LED, WFI between presses |
| [stm32u575/i2c](stm32u575/i2c) | LL | a 24xx EEPROM page write with acknowledge polling, configs swapping the device, a scripted TableTarget |
| [stm32u575/i2c_mock](stm32u575/i2c_mock) | LL | a temperature sensor written in Python as an `I2CTarget`: scripted readings, the configuration word the firmware wrote, NACK injection and recovery |
| [stm32u575/iwdg](stm32u575/iwdg) | HAL | IWDG refresh, a watchdog reset request 500 ms after the last refresh, IWDGRSTF after the reset |
| [stm32u575/linked_uart](stm32u575/linked_uart) | HAL DMA | a `popcorn.System` of a controller and two responders, each its own firmware and machine, on one virtual clock: UART links with a latency, receive-to-idle on a circular GPDMA ring, the `system` fixture, the link tap, an injected and a replaced frame, a watchdog reset of one member, a system snapshot, the same run at any slicing, quantum or CPU count |
| [stm32u575/pwr_flash](stm32u575/pwr_flash) | LL | voltage range 1 and 4 wait states before 160 MHz, a page erase and a quad-word program read back through `machine.mem`, PGSERR from a store without PG, a too low wait-state count failing a strict run |
| [stm32u575/python_model](stm32u575/python_model) | HAL IT | the RNG, which popcorn does not model, served by a `popcorn.model.Model` written in Python: register handlers, a scheduled DRDY, the interrupt line, a seed from the project |
| [stm32u575/rcc](stm32u575/rcc) | LL | HSE bypass to a 160 MHz PLL1, voltage range, flash latency, SysTick period in virtual time |
| [stm32u575/sai](stm32u575/sai) | HAL DMA | a 48 kHz I2S sine on SAI1 block A from a circular GPDMA linked list, frames captured from Python with their start times, half and complete callbacks |
| [stm32u575/sai_codec](stm32u575/sai_codec) | HAL DMA | an I2C codec written in Python clocking a slave SAI1 pair through `DeviceBoard.set_audio_clock` and `AudioOptions(clocked_by=...)`, two 2D circular GPDMA nodes interleaving and splitting an `int16_t[2][64]`, per-2D-block callbacks, a snapshot mid-stream |
| [stm32u575/spi](stm32u575/spi) | FreeRTOS, HAL init, LL driver | AT25SF NOR flash on the hardware SS output, an interrupt-driven transfer from a task, an application linked behind a bootloader offset |
| [stm32u575/spi_mock](stm32u575/spi_mock) | HAL | a two-channel ADC written in Python as a `SpiTarget` on a software chip select: samples from a function of virtual time, chip select framing, `Machine(devices=)` next to the project |
| [stm32u575/tim](stm32u575/tim) | LL | TIM2 1 kHz update interrupt toggling PA5, TIM16 one-pulse after 50 ms, update period in virtual time |
| [stm32u575/uart_mock](stm32u575/uart_mock) | HAL IT | an AT modem written in Python on the host end of USART2: replies in virtual time from the test's coroutine, an ERROR reply, a reply that never comes and the firmware's timeout |
| [stm32u575/usart](stm32u575/usart) | HAL IT | a printf banner and a receive-to-idle line echo, frame timing at the BRR rate |
| [stm32u575/usb_audio](stm32u575/usb_audio) | HAL, TinyUSB | a UAC2 headset on `Periph(host=UsbHostOptions())`: the Linux snd-usb-audio probe sequence, a 16 kHz mono sine looped from `host.audio().speaker` to `mic.read_frames` with frame times, `drain`, `stop`, a snapshot taken mid-stream |
| [stm32u575/usb_cdc](stm32u575/usb_cdc) | HAL, TinyUSB | a CDC ACM and two HID composite device enumerated by the virtual host: `wait_configured`, descriptors, a `host.cdc()` echo across a snapshot and an unplug, the same through `m.board.pty("OTG_FS")`, which `Periph(connect="pty")` does at build, HID reports with their times |

Every example has the same layout:

- `firmware/` the C sources, as a CubeMX project would have them
- `prebuilt/<name>.elf` the committed firmware, used as is without arm-none-eabi-gcc
- `conftest.py` the `popcorn_project` fixture: chip, image, boot, board devices
- `test_*.py` the tests. pytest loads the `popcorn.pytest` plugin by its entry point
- `pytest.ini` makes the example directory the pytest rootdir, so a copied example stands alone

`linked_uart` runs two firmware images on three machines. It keeps its projects in
`linked_projects.py` and has one test folder per image plus `system/` for the linked system. Its
README shows the layout.

## Declaring a project

A test project is a `popcorn.Project` returned by a session fixture named `popcorn_project` in a
`conftest.py` above the tests. The nearest conftest wins, so each example directory declares its
own. Paths are absolute, built from the conftest's location. This is the whole of
`stm32u575/usb_cdc/conftest.py`:

```python
from pathlib import Path

import popcorn
import pytest
from popcorn.board import UsbHostOptions

HERE = Path(__file__).parent

PROJECT = popcorn.Project(
    name="usb-cdc",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/usb_cdc.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={
        "OTG_FS": popcorn.Periph(host=UsbHostOptions()),
    },
)


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.Project:
    return PROJECT
```

- `strict=True` fails the run on a register access no model serves, as in `crc`.
- `Periph(devices=[Device(cls, address=0x50, **params)])` attaches a bus device, `Periph(model=Use(cls, **params))` replaces a peripheral model, as in `i2c_mock` and `python_model`.
- `rcc=RccOptions(...)` and `supply=SupplyOptions(...)` set the board clock and supply, as in `rcc` and `adc_watchdog`.

Named variants of the project come from `with_configs`. This is the `i2c_mock` conftest:

```python
PROJECT = BASE.with_configs(
    hot=BASE.replace(
        board={
            "I2C1": popcorn.Periph(devices=[popcorn.Device(TempSensor, address=0x48, readings=HOT)])
        }
    )
)
```

A test picks one with `@pytest.mark.popcorn(config="hot")`, `machine_factory("hot")` or the
`--popcorn-config=hot` option. `machine_factory(project=other)` boots another `Project`.

`popcorn_project` is always the base project. A test that boots machines itself takes
`popcorn_selected_project` instead: the same project with the marker's config applied, else
`--popcorn-config`'s, else the base. It is the project `machine` builds from. With
`m = project.machine()`, `await project.boot_machine(m)` resets and boots a machine you built
yourself.

Migrating from the TOML project stack:

- `popcorn_project` returned the project with `--popcorn-config` applied. It returns the base now,
  use `popcorn_selected_project` for the applied one.
- `--popcorn-project` is removed, the project comes from the `popcorn_project` fixture.
- `boot(machine)` of the TOML-era `Project` is `boot_machine(machine)`.
- `description()` of the TOML-era `Project` is `machine_description()`.

Run one example from the repository root:

```sh
uv run pytest -q examples/stm32u575/spi
```

Or from its own directory:

```sh
cd examples/stm32u575/spi && uv run pytest -q
```

Or every example in one session, each test booting the project of its directory:

```sh
uv run pytest -q examples
```

`uv run pcons run check` runs every example as its own gate stage, after the `tests/python`
suite, then all of them in one session with `-n 4`. The examples double as model regression tests: each one
fails when the model it drives breaks.

With arm-none-eabi-gcc on PATH, `uv run pcons -B build -j4` rebuilds every example's firmware.
`uv run pcons -B build -j4 firmware-check` then compares each rebuilt ELF with its prebuilt copy,
loadable content only.
