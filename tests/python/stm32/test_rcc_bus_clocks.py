"""Every instance of every shipped part has the RCC bus clock enable that
`tests/data/periph/rcc_bus_clocks.json` gives for its family, or is listed there as a block with
none. A fresh machine gates each instance whose enable resets to 0, setting the bit ungates it and
clearing it gates it again."""

from __future__ import annotations

import json
from pathlib import Path

import popcorn
import pytest
from popcorn.stm32 import mcu, shipped_parts

DATA = Path(__file__).parents[2] / "data" / "periph" / "rcc_bus_clocks.json"
TABLES: dict[str, dict[str, list[str]]] = json.loads(DATA.read_text())
ENABLES: dict[str, dict[str, str]] = json.loads(DATA.read_text())["enable"]


def family(part: str) -> str:
    return part[:7]


@pytest.mark.parametrize("part", shipped_parts())
def test_every_instance_is_gated_by_its_rcc_enable_or_listed(part: str) -> None:
    line = family(part)
    no_enable = set(TABLES["no_enable"][line])
    at_reset = set(TABLES["enabled_at_reset"][line])
    machine = popcorn.MachineDescription(part=part, images=[]).machine()
    rcc = machine.periph.RCC
    wrong: list[str] = []
    for name in (p.name for p in mcu(part).peripherals if p.alias_of is None):
        view = machine.periph[name]
        if name in no_enable:
            if view.clock_gated:
                wrong.append(f"{name} gated")
            continue
        if name not in ENABLES[line]:
            wrong.append(f"{name} has no expected enable")
            continue
        register, field = ENABLES[line][name].split(".")
        if view.clock_gated == (name in at_reset):
            wrong.append(f"{name} at reset")
        if name in at_reset:
            continue
        setattr(rcc[register], field, 1)
        if view.clock_gated:
            wrong.append(f"{name} gated with {register}.{field} 1")
        setattr(rcc[register], field, 0)
        if not view.clock_gated:
            wrong.append(f"{name} ungated with {register}.{field} 0")
    assert wrong == []


def test_the_lists_name_only_instances_some_shipped_part_has() -> None:
    present: dict[str, set[str]] = {}
    for part in shipped_parts():
        present.setdefault(family(part), set()).update(p.name for p in mcu(part).peripherals)
    for key in ("no_enable", "enabled_at_reset"):
        for line, names in TABLES[key].items():
            assert set(names) <= present[line], line
    for line, enables in ENABLES.items():
        assert set(enables) <= present[line], line
