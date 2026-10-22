from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn import StopReason

ELF = Path(__file__).parents[3] / "examples/stm32u575/gpio/prebuilt/gpio_button.elf"

BUTTON = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(ELF)],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def button() -> popcorn.MachineDescription:
    return BUTTON


# --8<-- [start:marker]
pytestmark = pytest.mark.popcorn(budget=200)


async def test_the_module_marker_bounds_every_run(button: popcorn.Machine) -> None:
    before = button.executed
    stop = await button.run()
    assert stop.reason is StopReason.BUDGET
    assert button.executed - before == 200


@pytest.mark.popcorn(budget=1_000)
async def test_the_closest_marker_wins_whole(button: popcorn.Machine) -> None:
    before = button.executed
    stop = await button.run()
    assert stop.reason is StopReason.BUDGET
    assert button.executed - before == 1_000


# --8<-- [end:marker]
