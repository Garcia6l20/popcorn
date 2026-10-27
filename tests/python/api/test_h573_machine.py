"""The STM32H573 description builds a Machine: every model whose IP matches constructs on it."""

from __future__ import annotations

import asyncio
from pathlib import Path

import popcorn
import pytest
from popcorn.stm32 import mcu
from popcorn.stm32.description import DescriptionError
from popcorn.units import ms

BOOT_LL = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "h573" / "boot_ll.elf"


@pytest.fixture(scope="module")
def h573() -> popcorn.PartDescription:
    try:
        return mcu("STM32H573AIIx")
    except DescriptionError as error:
        pytest.skip(f"no STM32H573 description: {error}")


def test_the_h573_machine_builds_without_firmware(h573: popcorn.PartDescription) -> None:
    machine = popcorn.Machine(h573)
    assert machine.mem.read(0x20000000, 4) == bytes(4)


def test_a_machine_description_of_the_h573_builds_a_machine() -> None:
    description = popcorn.MachineDescription(part="STM32H573AIIx", images=[])
    try:
        machine = description.machine()
    except (DescriptionError, popcorn.MissingInput) as error:
        pytest.skip(f"no STM32H573 description: {error}")
    assert machine.mem.read(0x08FFF80C, 2) == (2048).to_bytes(2, "little")


def test_gpioi_pin_12_is_refused_at_the_call(h573: popcorn.PartDescription) -> None:
    machine = popcorn.Machine(h573)
    gpioi = machine.periph.GPIOI
    for pin in (12, 15):
        with pytest.raises(popcorn.ConfigError, match="a pin of the port, 0 to 11"):
            gpioi.pin(pin).level = True
    gpioi.pin(11).level = True
    machine.periph.GPIOH.pin(15).level = True


def test_gpioi_pin_12_edges_are_refused_at_the_call_parked_and_running(
    h573: popcorn.PartDescription,
) -> None:
    machine = popcorn.Machine(h573, image=popcorn.Image.elf(BOOT_LL))
    machine.reset()
    gpioi = machine.periph.GPIOI
    at = machine.time + ms(1)
    for pin in (12, 15):
        with pytest.raises(popcorn.ConfigError, match="a pin of the port, 0 to 11"):
            gpioi.pin(pin).drive_at(True, at=at)

    async def running() -> None:
        async with machine.running():
            for pin in (12, 15):
                with pytest.raises(popcorn.ConfigError, match="a pin of the port, 0 to 11"):
                    gpioi.pin(pin).drive_edges([(at, True)])
            gpioi.pin(11).drive_at(True, at=at)

    asyncio.run(running())
    asyncio.run(machine.run(for_=ms(2)))
