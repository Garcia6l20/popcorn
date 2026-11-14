"""Every STM32U535 and STM32U545 part carries the LPGPIO1 pads of the pins it bonds."""

import re

import pytest
from popcorn.stm32.description._builder import build_description_with_report
from popcorn.stm32.description._sources import find_sources

from popcorn_build.part_sweep import db_parts

pytestmark = pytest.mark.timeout(120)

PADS = {
    0: ("A", 1),
    1: ("A", 3),
    2: ("A", 6),
    3: ("B", 1),
    4: ("B", 10),
    5: ("C", 2),
    6: ("D", 13),
    7: ("D", 2),
    8: ("C", 10),
    9: ("B", 0),
    10: ("C", 12),
    11: ("B", 3),
    12: ("B", 4),
    13: ("E", 0),
    14: ("E", 2),
    15: ("E", 3),
}
PARTS = db_parts(["STM32U535*", "STM32U545*"])


def pad_numbers(part: str) -> tuple[set[int], set[int]]:
    description = build_description_with_report(find_sources(part))[0]
    ports = {p.name: p for p in description.peripherals if p.name.startswith("GPIO")}
    added = {
        int(match.group(1))
        for port in ports.values()
        for alternate in port.alternates or []
        if alternate.af == 11 and (match := re.fullmatch(r"lpgpio1_p(\d+)", alternate.signal))
    }
    bonded = {
        number
        for number, (port, pin) in PADS.items()
        if f"GPIO{port}" in ports and (ports[f"GPIO{port}"].bonded or 0) >> pin & 1
    }
    return added, bonded


def test_the_db_holds_the_parts_of_both_lines() -> None:
    assert len(PARTS) >= 40
    for part in ("STM32U535CETxQ", "STM32U535NEYxQ", "STM32U545VETx", "STM32U545VEIx"):
        assert part in PARTS


@pytest.mark.parametrize("part", PARTS)
def test_the_lpgpio1_pads_follow_the_bonded_pins(part: str) -> None:
    added, bonded = pad_numbers(part)
    assert added == bonded


@pytest.mark.parametrize(
    ("part", "count"),
    [
        ("STM32U535CETx", 8),
        ("STM32U535CETxQ", 7),
        ("STM32U535NEYxQ", 8),
        ("STM32U535JEYxQ", 8),
        ("STM32U545RETx", 12),
        ("STM32U545VETx", 16),
        ("STM32U545VEIx", 16),
    ],
)
def test_the_packages_carry_their_pad_counts(part: str, count: int) -> None:
    assert len(pad_numbers(part)[0]) == count
