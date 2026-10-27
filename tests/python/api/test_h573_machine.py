"""The STM32H573 description builds a Machine: every model whose IP matches constructs on it."""

from __future__ import annotations

import asyncio
from pathlib import Path

import popcorn
import pytest
from popcorn.stm32 import mcu
from popcorn.stm32.description import DescriptionError
from popcorn.units import ms

H5_EXTERNAL = ", ".join(str(n) for n in (*range(16), 18, 19))
RCC_LL = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "h573" / "rcc_ll.elf"


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
    machine = popcorn.Machine(h573, image=popcorn.Image.elf(RCC_LL))
    machine.reset()
    machine.reg.pc = machine.sym("idle")
    gpioi = machine.periph.GPIOI
    gpioi.MODER.MODE11 = 0
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
    assert gpioi.IDR.ID11 == 1


@pytest.mark.parametrize("adc", ["ADC1", "ADC2"])
def test_the_h573_adc_inputs_are_the_external_channels_of_its_model(
    h573: popcorn.PartDescription, adc: str
) -> None:
    machine = popcorn.Machine(h573)
    unit = getattr(machine.periph, adc)
    for channel in (*range(16), 18, 19):
        unit.input(channel).volts = channel / 10
    for channel in (*range(16), 18, 19):
        assert unit.input(channel).volts == pytest.approx(channel / 10)
    for channel in (16, 17, 20, -1):
        with pytest.raises(popcorn.ConfigError) as caught:
            unit.input(channel)
        assert caught.value.expected == f"an external channel, one of {H5_EXTERNAL}"
