"""Every shipped STM32L4 DIE415 part reaches the end of its flash, SRAM1 and SRAM2 and the pins of
its package, boots its flash bank 2 at the bank 2 base of its size, and reads its DEV_ID, with no
firmware.

The flash size comes from the ordering code: C, E and G give 256, 512 and 1024 KiB, two banks of
half the size each, bank 2 at the flash base plus half the size, RM0351 3.3.1 Tables 8 to 10. Every
DIE415 part has 96 KiB SRAM1 at 0x20000000 and 32 KiB SRAM2 at 0x10000000, RM0351 2.4, and
neither is aliased past its end, RM0351 Table 6 and the 2.4 alias note for STM32L49x/L4Ax only.
The DEV_ID is 0x415, RM0351 48.6.1. The refused pins come from the description's own `bonded`
masks.
"""

from __future__ import annotations

import popcorn
import pytest
from popcorn import OptionBytes
from popcorn.stm32 import mcu, shipped_parts

L4_PARTS = [part for part in shipped_parts() if part.startswith("STM32L4")]
FLASH_KIB = {"C": 256, "E": 512, "G": 1024}
FLASH = "Flash"
FLASH_START = 0x0800_0000
SRAM1 = ("SRAM1", 0x2000_0000, 96 * 1024)
SRAM2 = ("SRAM2", 0x1000_0000, 32 * 1024)
IDR = 0x10
DBGMCU_IDCODE = 0xE004_2000
DEV_ID_MASK = 0xFFF
DEV_ID = 0x415
LINES = {"L471": 10, "L475": 6, "L476": 19, "L485": 2, "L486": 5}
"""Order codes of each DIE415 line, by the CubeMX RefName expansion."""


def build(part: str, option_bytes: OptionBytes | None = None) -> popcorn.Machine:
    if option_bytes is None:
        machine = popcorn.MachineDescription(part=part, images=[]).machine()
    else:
        machine = popcorn.Machine(mcu(part), option_bytes=option_bytes)
    machine.reset()
    return machine


def covered(machine: popcorn.Machine, address: int) -> bool:
    description = machine.description
    return any(r.start <= address < r.start + r.size for r in description.regions) or any(
        p.base <= address < p.base + p.size for p in description.peripherals
    )


def region(machine: popcorn.Machine, name: str) -> tuple[int, int]:
    found = next(r for r in machine.description.regions if r.name == name)
    return found.start, found.start + found.size


def round_trips_to_its_end(machine: popcorn.Machine, name: str) -> None:
    """The first and last words of region @p name round-trip and the next address is unmapped."""
    start, end = region(machine, name)
    for address, value in ((start, 0x5A3C_C3A5), (end - 4, 0xA5C3_0F1E)):
        machine.mem.u32[address] = value
        assert machine.mem.u32[address] == value, name
    assert not covered(machine, end), name
    with pytest.raises(popcorn.BusAccessError):
        machine.mem.u32[end]


def test_the_shipped_set_holds_the_42_die415_names() -> None:
    assert {line: sum(p[5:9] == line for p in L4_PARTS) for line in LINES} == LINES
    assert len(L4_PARTS) == sum(LINES.values())


@pytest.mark.parametrize("part", L4_PARTS)
def test_the_flash_matches_the_ordering_code_and_ends_where_it_says(part: str) -> None:
    machine = build(part)
    assert region(machine, FLASH) == (FLASH_START, FLASH_START + FLASH_KIB[part[10]] * 1024)
    round_trips_to_its_end(machine, FLASH)


@pytest.mark.parametrize("part", L4_PARTS)
def test_bank_2_boots_at_half_the_flash_size(part: str) -> None:
    machine = build(part, OptionBytes(swap_bank=True))
    bank2 = FLASH_START + FLASH_KIB[part[10]] * 1024 // 2
    machine.mem.u32[bank2] = 0x2000_4000
    machine.mem.u32[bank2 + 4] = 0x0800_0101
    machine.mem.u32[FLASH_START] = 0x2000_8000
    machine.mem.u32[FLASH_START + 4] = 0x0800_0201
    machine.reset()
    assert (machine.reg.sp, machine.reg.pc) == (0x2000_4000, 0x0800_0100)


@pytest.mark.parametrize("part", L4_PARTS)
def test_the_dbgmcu_dev_id_is_0x415(part: str) -> None:
    assert build(part).mem.u32[DBGMCU_IDCODE] & DEV_ID_MASK == DEV_ID


@pytest.mark.parametrize("part", L4_PARTS)
def test_the_last_word_of_sram1_and_of_sram2_round_trips(part: str) -> None:
    machine = build(part)
    for name, start, size in (SRAM1, SRAM2):
        assert region(machine, name) == (start, start + size)
        round_trips_to_its_end(machine, name)


@pytest.mark.parametrize("part", L4_PARTS)
def test_every_bonded_pin_drives_and_every_other_pin_refuses(part: str) -> None:
    machine = build(part)
    ports = [
        p
        for p in machine.description.peripherals
        if p.name.startswith("GPIO") and p.alias_of is None
    ]
    assert ports
    for port in ports:
        bonded = 0xFFFF if port.bonded is None else port.bonded
        machine.mem.u32[port.base] = 0
        view = getattr(machine.periph, port.name)
        for number in range(16):
            if bonded >> number & 1:
                pin = view.pin(number)
                for level in (True, False, True):
                    pin.level = level
                    assert bool(machine.mem.u32[port.base + IDR] >> number & 1) == level
            else:
                with pytest.raises(popcorn.ConfigError) as refused:
                    view.pin(number).level = True
                assert refused.value.key == "pin"
