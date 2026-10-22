from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn.units import ms

ELF = Path(__file__).parents[2] / "examples/stm32u575/gpio/prebuilt/gpio_button.elf"


# --8<-- [start:boot]
BUTTON = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(ELF)],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def button() -> popcorn.MachineDescription:
    return BUTTON


async def test_the_led_starts_off(button: popcorn.Machine) -> None:
    await button.run(for_=ms(1))
    assert not button.periph.GPIOA.pin(5).level
    assert button.var("presses").value == 0


# --8<-- [end:boot]
