"""A firmware-less Machine on the description of each STM32L1 category."""

import popcorn
import pytest
from popcorn.stm32.description._binder import dump_json
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._errors import MissingSource
from popcorn.stm32.description._sources import find_sources

from ..conftest import require_pin_data

FLASH_KIB = {
    "STM32L152RBTx": 128,
    "STM32L152RBTxA": 128,
    "STM32L152RCTx": 256,
    "STM32L152RCTxA": 256,
    "STM32L152RETx": 512,
    "STM32L152ZDTx": 384,
    "STM32L152VDTxX": 384,
}
CODES = {"STM32L152RCTxA": 1, "STM32L152ZDTx": 0}


def _machine(name: str) -> popcorn.Machine:
    require_pin_data()
    try:
        sources = find_sources(name)
    except MissingSource as exc:
        pytest.skip(f"{name} sources not found: {exc}")
    return popcorn.Machine(popcorn.parse_description(dump_json(build_description(sources)), name))


@pytest.mark.timeout(120)
@pytest.mark.parametrize("name", sorted(FLASH_KIB))
def test_a_machine_attaches_and_serves_the_device_signature(name: str) -> None:
    machine = _machine(name)
    cat12 = name in {"STM32L152RBTx", "STM32L152RBTxA"}
    size, uid = (0x1FF8_004C, 0x1FF8_0050) if cat12 else (0x1FF8_00CC, 0x1FF8_00D0)
    expected = CODES.get(name, FLASH_KIB[name])
    assert machine.mem.read(size, 2) == expected.to_bytes(2, "little")
    assert machine.mem.read(uid, 8) == bytes.fromhex("14003200014e524f")
    assert machine.mem.read(uid + 0x14, 4) == bytes.fromhex("43504f50")
