# --8<-- [start:boot]
from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn.units import ms

ELF = Path(__file__).parents[2] / "examples/stm32u575/gpio/prebuilt/gpio_button.elf"

BUTTON = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(ELF)],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def button() -> popcorn.MachineDescription:
    return BUTTON


async def test_a_press_turns_the_led_on(button: popcorn.Machine) -> None:
    pin = button.periph.GPIOC.pin(13)
    for level in (1, 0, 1):
        pin.level = level
        stop = await button.run(for_=ms(1))
        assert stop.reason is popcorn.StopReason.TIME
    assert button.periph.GPIOA.pin(5).level
    assert button.var("presses").value == 1


# --8<-- [end:boot]
