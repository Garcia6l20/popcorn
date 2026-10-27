# popcorn examples

Each directory is a firmware project for one chip with the pytest suite that runs it on popcorn.
Copy one, build it, run its tests, then read its `test_*.py` to see how a test drives the machine.

| example | firmware style | shows |
| --- | --- | --- |
| [stm32h573/gpdma](stm32h573/gpdma) | HAL DMA | UART9 TX over GPDMA1, a normal block then a two-node linked list, request 38, frames back to back while the CPU sleeps in WFI |
| [stm32h573/pwr_flash](stm32h573/pwr_flash) | LL | VOS0 and 5 wait states before 250 MHz, a sector erase with SER, SNB and BKSEL and a quad-word program read back through `machine.mem`, EOP cleared in FLASH_NSCCR, PGSERR from a store without PG, a too low wait-state count failing a strict run |
| [stm32h573/rcc](stm32h573/rcc) | LL | HSI to a 250 MHz PLL1 with a fractional N, VOS0, flash latency, SysTick period in virtual time |
| [stm32h573/spi](stm32h573/spi) | FreeRTOS, HAL init, LL driver | AT25SF NOR flash on SPI4 and its hardware SS output, a GPDMA1 transfer on channels 0 and 1 from a task, an application linked behind a bootloader offset |
| [stm32h573/usart](stm32h573/usart) | HAL IT | a printf banner and a receive-to-idle line echo on UART9, PG0 and PG1 on AF11, frame timing at the BRR rate |
| [stm32u575/adc](stm32u575/adc) | LL | ADC1 power-up, calibration, VREFINT and the temperature sensor read against the factory words, timed conversions |
| [stm32u575/adc_watchdog](stm32u575/adc_watchdog) | HAL DMA | a TIM15-triggered 4-channel ADC1 scan, 8x oversampling, circular GPDMA, AWD3 on one input: inputs driven with `ADC1.input(n).volts`, `SupplyOptions(vdda_mv=3300)`, the strict threshold comparison, scan timing |
| [stm32u575/crc](stm32u575/crc) | HAL | the CRC unit from `HAL_CRC_Calculate` and `HAL_CRC_Accumulate`: CRC-32/MPEG-2 on words, CRC-32/ISO-HDLC with input and output reversal, CRC-16/IBM-3740, checked against a bit-serial Python reference |
| [stm32u575/gpdma](stm32u575/gpdma) | HAL DMA | USART1 TX over GPDMA1, a normal block then a two-node linked list, frames back to back while the CPU sleeps in WFI |
| [stm32u575/gpio](stm32u575/gpio) | HAL | a host-driven button on an EXTI falling edge toggling an LED, WFI between presses |
| [stm32u575/i2c](stm32u575/i2c) | LL | a 24xx EEPROM page write with acknowledge polling, configs swapping the device, a scripted TableTarget |
| [stm32u575/i2c_mock](stm32u575/i2c_mock) | LL | a temperature sensor written in Python as an `I2CTarget`: scripted readings, the configuration word the firmware wrote, NACK injection and recovery |
| [stm32u575/iwdg](stm32u575/iwdg) | HAL | IWDG refresh, a watchdog reset request 500 ms after the last refresh, IWDGRSTF after the reset |
| [stm32u575/linked_uart](stm32u575/linked_uart) | HAL DMA | a `popcorn.System` of a controller and two responders, each its own firmware and machine, on one virtual clock: UART links with a latency, receive-to-idle on a circular GPDMA ring, the `system` fixture, the link tap, an injected and a replaced frame, a watchdog reset of one member, a cable pulled and plugged back with `UartTap.cut`, a system snapshot, the same run at any slicing, quantum or CPU count |
| [stm32u575/native_plugin](stm32u575/native_plugin) | HAL IT | a native plugin: the RNG served by a C++ model selected with `PLUGIN.model("rng")`, a C++ SPI ADC from `PLUGIN.module()` on a software chip select, the dev-mode build and cache |
| [stm32u575/pdm_mics](stm32u575/pdm_mics) | HAL DMA | two PDM microphones on MDF1 through `HAL_MDF_Init`, `HAL_MDF_AcqStart_DMA` and `HAL_MDF_GenerateTrgo`: sines fed with `m.periph.MDF1.mic(sitf, edge).write_pcm`, two interleaved filters in one circular GPDMA ring of 16-bit MSBs, the filter outputs with their times from `output(n)`, LF_MASTER at exactly 2 x CCK against RM0456 Table 374, a strict run, saturation reaching the error callback |
| [stm32u575/pwr_flash](stm32u575/pwr_flash) | LL | voltage range 1 and 4 wait states before 160 MHz, a page erase and a quad-word program read back through `machine.mem`, PGSERR from a store without PG, a too low wait-state count failing a strict run |
| [stm32u575/python_model](stm32u575/python_model) | HAL IT | the RNG served by a `popcorn.model.Model` written in Python in place of the built-in model: register handlers, a scheduled DRDY, the interrupt line, a seed from the description |
| [stm32u575/rcc](stm32u575/rcc) | LL | HSE bypass to a 160 MHz PLL1, voltage range, flash latency, SysTick period in virtual time |
| [stm32u575/rng](stm32u575/rng) | LL | a 64-byte key from the built-in RNG: words predicted with `rng_words`, a `seeded` config with `RngOptions`, chosen words queued, a seed error after 8 words, a clock error, a stuck word, HSI48 left off |
| [stm32u575/sai](stm32u575/sai) | HAL DMA | a 48 kHz I2S sine on SAI1 block A from a circular GPDMA linked list, frames captured from Python with their start times, half and complete callbacks |
| [stm32u575/sai_codec](stm32u575/sai_codec) | HAL DMA | an I2C codec written in Python clocking a slave SAI1 pair through `DeviceBoard.set_audio_clock` and `AudioOptions(clocked_by=...)`, two 2D circular GPDMA nodes interleaving and splitting an `int16_t[2][64]`, per-2D-block callbacks, a snapshot mid-stream |
| [stm32u575/spi](stm32u575/spi) | FreeRTOS, HAL init, LL driver | AT25SF NOR flash on the hardware SS output, an interrupt-driven transfer from a task, an application linked behind a bootloader offset |
| [stm32u575/spi_mock](stm32u575/spi_mock) | HAL | a two-channel ADC written in Python as a `SpiTarget` on a software chip select: samples from a function of virtual time, chip select framing, `machine_factory.booted(description, devices=)` |
| [stm32u575/tim](stm32u575/tim) | LL | TIM2 1 kHz update interrupt toggling PA5, TIM16 one-pulse after 50 ms, update period in virtual time |
| [stm32u575/trustzone](stm32u575/trustzone) | CMSIS, CMSE | a secure and Non-secure pair with TZEN=1: the secure binary embedded in the Non-secure ELF, a symbols-only `Elf(loaded=False)`, `OptionBytes`, SG veneers probed by `state=`, SecureFault AUVIOL and INVEP and the BFHFNMINS HardFault targets with `faults="handler"`, a Secure MPCBB block, both SysTick timers, the FP context across BLXNS, a snapshot inside a Non-secure callback |
| [stm32u575/uart_mock](stm32u575/uart_mock) | HAL IT | an AT modem written in Python on the host end of USART2: replies in virtual time from the test's coroutine, an ERROR reply, a reply that never comes and the firmware's timeout |
| [stm32u575/usart](stm32u575/usart) | HAL IT | a printf banner and a receive-to-idle line echo, frame timing at the BRR rate |
| [stm32u575/usb_audio](stm32u575/usb_audio) | HAL, TinyUSB | a UAC2 headset on `Periph(host=UsbHostOptions())`: the Linux snd-usb-audio probe sequence, a 16 kHz mono sine looped from `host.audio().speaker` to `mic.read_frames` with frame times, `drain`, `stop`, a snapshot taken mid-stream |
| [stm32u575/usb_cdc](stm32u575/usb_cdc) | HAL, TinyUSB | a CDC ACM and two HID composite device enumerated by the virtual host: `wait_configured`, descriptors, a `host.cdc()` echo across a snapshot and an unplug, the same through `m.board.pty("OTG_FS")`, which `Periph(connect="pty")` does at build, HID reports with their times |

Every example has the same layout:

- `firmware/` the C sources, as a CubeMX project would have them
- `prebuilt/<name>.elf` the committed firmware, used as is without arm-none-eabi-gcc
- `conftest.py` the machine fixtures: chip, image, boot, board devices
- `test_*.py` the tests. pytest loads the `popcorn.pytest` plugin by its entry point, installed
  with the `popcorn[pytest]` extra
- `pytest.ini` makes the example directory the pytest rootdir, so a copied example stands alone

`linked_uart` runs two firmware images on three machines. It declares both descriptions, three
machine fixtures and the linked system in one root `conftest.py`, with one test folder per image
plus `system/` for the linked system. Its README shows the layout.

## Declaring a machine

A machine fixture is a function in a `conftest.py` above the tests that returns a
`popcorn.MachineDescription`, decorated with `@popcorn.pytest.machine`. The fixture is named after
the function. The nearest conftest wins, so each example directory declares its own. Paths are
absolute, built from the conftest's location. This is the whole of `stm32u575/usb_cdc/conftest.py`:

```python
"""MachineDescription of the usb_cdc example."""

from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn.board import UsbHostOptions

HERE = Path(__file__).parent

USB_CDC = popcorn.MachineDescription(
    name="usb-cdc",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/usb_cdc.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={
        "OTG_FS": popcorn.Periph(host=UsbHostOptions()),
    },
)


@popcorn.pytest.machine
def usb_cdc() -> popcorn.MachineDescription:
    return USB_CDC
```

A test takes the machine by that name, `Machine` from `popcorn`:

```python
async def test_the_host_enumerates_the_device_from_boot(usb_cdc: Machine) -> None: ...
```

The machine boots once per description and process, then every test gets it restored from the
boot snapshot. The plugin owns the boot, the restore, the teardown, the failure report and the
traces. The machine is named after the fixture in logs and errors.

- `strict=True` fails the run on a register access no model serves, as in `crc`.
- `faults="handler"` makes a processor fault enter the firmware's fault handler instead of stopping the run, as in `trustzone`.
- `Periph(devices=[Device(cls, address=0x50, **params)])` attaches a bus device, `Periph(model=Use(cls, **params))` replaces a peripheral model, as in `i2c_mock` and `python_model`.
- `rcc=RccOptions(...)` and `supply=SupplyOptions(...)` set the board clock and supply, as in `rcc` and `adc_watchdog`.
- `rng=RngOptions(seed=...)` sets the words of the built-in RNG, as in `rng`.

Registers go by their bare names, `m.periph.GPIOA.MODER`. The SVD names, `GPIO_MODER`, answer as
aliases, so examples written either way run.

## Configs

Named variants of a description come from `with_configs`. The config is chosen on the decorator,
one fixture per variant. This is the end of the `i2c_mock` conftest, `BASE` being the cold board:

```python
I2C_MOCK = BASE.with_configs(
    hot=BASE.replace(
        board={
            "I2C1": popcorn.Periph(devices=[popcorn.Device(TempSensor, address=0x48, readings=HOT)])
        }
    )
)


@popcorn.pytest.machine
def i2c_mock() -> popcorn.MachineDescription:
    return I2C_MOCK


@popcorn.pytest.machine(config="hot")
def i2c_mock_hot() -> popcorn.MachineDescription:
    return I2C_MOCK
```

A test takes `i2c_mock` for the base board and `i2c_mock_hot` for the hot one. Both forms of the
decorator work, `@popcorn.pytest.machine` and `@popcorn.pytest.machine(config=...)`. The config
is set on the decorator or on the description only. The `popcorn` marker does not take it.

## Several fixtures

Any number of fixtures fit in one conftest. Each has its own machine, so two fixtures of one
description in one test are two machines restored from one boot. This is the `linked_uart`
conftest, which also links the three machines:

```python
@popcorn.pytest.machine
def controller() -> popcorn.MachineDescription:
    return CONTROLLER


@popcorn.pytest.machine
def responder1() -> popcorn.MachineDescription:
    return RESPONDER


@popcorn.pytest.machine
def responder2() -> popcorn.MachineDescription:
    return RESPONDER


@popcorn.pytest.system
def system() -> popcorn.SystemDescription:
    return SYSTEM
```

`@popcorn.pytest.system` boots a `popcorn.System` of a `popcorn.SystemDescription`, new for each
test. Its members keep their member keys as names and share the boots of the machine fixtures
that use the same descriptions, strictness and devices.

The decorated function is a full pytest fixture. It may request other fixtures, and both
decorators forward `params=` and `ids=` to pytest, with `request.param` in the function.

## Marker and factory

The `popcorn` marker takes `strict` and `budget` only, and applies them to every popcorn machine
of the test. `spi` sets an instruction budget for the whole module:

```python
pytestmark = pytest.mark.popcorn(budget=2_000_000)
```

A test that needs a machine of its own takes `machine_factory`. It builds new machines for the
test from a description given at each call, with `config=`, `devices=` and `booted=` as options.
This is the `i2c` test that boots a slow EEPROM beside its fixture machine:

```python
slow = await machine_factory.booted(i2c_description, config="slow-eeprom")
```

`i2c_description` is a plain `pytest.fixture` of the conftest that returns the description, as
`spi_mock_description` does in `spi_mock`.

The examples use the shipped `STM32U575QGIx` description and need no ST tools.

Run one example from the repository root:

```sh
uv run pytest -q examples/stm32u575/spi
```

Or from its own directory:

```sh
cd examples/stm32u575/spi && uv run pytest -q
```

Or every example in one session, each test booting the description of its directory:

```sh
uv run pytest -q examples
```

`uv run pcons run check` runs every example as its own gate stage, after the `tests/python`
suite, then all of them in one session with `-n 4`. The examples double as model regression tests: each one
fails when the model it drives breaks.

With arm-none-eabi-gcc on PATH, `uv run pcons -B build -j4` rebuilds every example's firmware.
`uv run pcons -B build -j4 firmware-check` then compares each rebuilt ELF with its prebuilt copy,
loadable content only.
