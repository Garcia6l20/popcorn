# popcorn

popcorn runs STM32 Cortex-M firmware on the Unicorn CPU emulator so it can be unit tested from
Python. A test loads the firmware ELF, calls single functions or boots the whole image with
emulated peripherals, drives inputs and asserts on outputs, memory, registers and peripheral
state. A pytest plugin boots once and restores a snapshot per test.

## Install

popcorn is installed from source and compiles its native core on your machine. It needs Linux
x86_64, Python 3.14 with its development headers, g++ 16.1 or later, cmake, ninja and git.

```sh
uv venv
uv pip install "popcorn[pytest] @ git+<repository-url>"
```

## Example

This test boots the gpio example firmware, presses the button on PC13 once and checks the LED on
PA5 and the press counter:

```python
import popcorn
import popcorn.pytest
from popcorn.units import ms

BUTTON = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf("gpio_button.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def button() -> popcorn.MachineDescription:
    return BUTTON


async def test_a_press_turns_the_led_on(button: popcorn.Machine) -> None:
    pin = button.periph.GPIOC.pin(13)
    for level in (1, 0, 1):
        pin.level = level
        await button.run(for_=ms(1))
    assert button.periph.GPIOA.pin(5).level
    assert button.var("presses").value == 1
```

## Documentation

The guide, the support tables, the API reference and the developer notes are in `docs/`. Build
them with `uv run python -m popcorn_build.docs`. The examples under `examples/` are complete
firmware projects with their tests.

## License

MIT, see `LICENSE`. Third-party components and their licences are listed in `NOTICE`. The shipped
STM32 part descriptions under `python/popcorn/stm32/data` derive from ST files under Apache-2.0 and
BSD-3-Clause, whose notices `NOTICE` carries.
