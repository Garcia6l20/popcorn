"""A firmware-less Machine on the description of one part of each STM32L4 DIE415 line."""

import popcorn
import pytest
from popcorn import OptionBytes
from popcorn.stm32 import mcu
from popcorn.stm32.description._binder import dump_json
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._errors import MissingSource
from popcorn.stm32.description._model import PartDescription
from popcorn.stm32.description._sources import find_sources

from ..conftest import require_pin_data

FLASH_KIB = {
    "STM32L471ZGTx": 1024,
    "STM32L475VGTx": 1024,
    "STM32L476RCTx": 256,
    "STM32L485JEYx": 512,
    "STM32L486ZGTx": 1024,
}
FLASH_SIZE = 0x1FFF_75E0


def _description(name: str) -> PartDescription:
    require_pin_data()
    try:
        sources = find_sources(name)
    except MissingSource as exc:
        pytest.skip(f"{name} sources not found: {exc}")
    return build_description(sources)


def _machine(
    description: PartDescription, option_bytes: OptionBytes | None = None
) -> popcorn.Machine:
    text = dump_json(description)
    return popcorn.Machine(
        popcorn.parse_description(text, description.part), option_bytes=option_bytes
    )


@pytest.mark.timeout(120)
@pytest.mark.parametrize("name", sorted(FLASH_KIB))
def test_a_machine_attaches_every_instance_and_serves_the_flash_size(name: str) -> None:
    description = _description(name)
    machine = _machine(description)
    machine.reset()
    assert machine.mem.read(FLASH_SIZE, 2) == FLASH_KIB[name].to_bytes(2, "little")
    for peripheral in description.peripherals:
        view = getattr(machine.periph, peripheral.name)
        primary = [register for register in peripheral.registers if register.alternate_of is None]
        first = min(primary, key=lambda register: register.offset)
        assert getattr(view, first.name).value == first.reset, (peripheral.name, first.name)


@pytest.mark.timeout(120)
def test_tzen_is_refused_on_an_stm32l476rctx_machine() -> None:
    description = _description("STM32L476RCTx")
    with pytest.raises(popcorn.ConfigError) as excinfo:
        _machine(description, option_bytes=OptionBytes(tzen=True))
    assert excinfo.value.key == "option_bytes.tzen"


def test_the_shipped_stm32l476rctx_builds_and_attaches() -> None:
    machine = popcorn.Machine(mcu("STM32L476RCTx"))
    machine.reset()
    assert machine.mem.read(FLASH_SIZE, 2) == (256).to_bytes(2, "little")
