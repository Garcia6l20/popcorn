from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn import StopReason
from popcorn.pytest import MachineFactory
from popcorn.units import ms

ELF = Path(__file__).parents[3] / "examples/stm32u575/gpio/prebuilt/gpio_button.elf"

# --8<-- [start:fixture]
BUTTON = popcorn.MachineDescription(
    name="button",
    part="STM32U575QGIx",
    images=[popcorn.Elf(ELF)],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def button() -> popcorn.MachineDescription:
    return BUTTON


# --8<-- [end:fixture]


# --8<-- [start:press]
async def press(machine: popcorn.Machine) -> None:
    pin = machine.periph.GPIOC.pin(13)
    for level in (1, 0, 1):
        pin.level = level
        stop = await machine.run(for_=ms(1))
        assert stop.reason is StopReason.TIME


# --8<-- [end:press]


# --8<-- [start:restored]
async def test_one_press(button: popcorn.Machine) -> None:
    assert button.var("presses").value == 0
    await press(button)
    assert button.var("presses").value == 1


async def test_two_presses(button: popcorn.Machine) -> None:
    assert button.var("presses").value == 0
    await press(button)
    await press(button)
    assert button.var("presses").value == 2


# --8<-- [end:restored]


# --8<-- [start:marker]
@pytest.mark.popcorn(budget=500)
async def test_a_small_budget_bounds_every_run(button: popcorn.Machine) -> None:
    stop = await button.run()
    assert stop.reason is StopReason.BUDGET


# --8<-- [end:marker]


# --8<-- [start:factory]
async def test_two_machines_of_one_description(machine_factory: MachineFactory) -> None:
    left = await machine_factory.booted(BUTTON)
    right = await machine_factory.booted(BUTTON)
    await press(left)
    assert left.var("presses").value == 1
    assert right.var("presses").value == 0


# --8<-- [end:factory]
