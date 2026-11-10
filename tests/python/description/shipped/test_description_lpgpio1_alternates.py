"""The LPGPIO1_Py alternates of the shipped STM32U575 and STM32U585 ports."""

import json
from typing import Any, cast

import pytest
from popcorn.stm32.description._shipped import shipped_names

from ..lpgpio1_pads import LPGPIO1_PADS
from .composed import composed

U5_PARTS = [n for n in shipped_names() if n.startswith(("STM32U575", "STM32U585"))]


def _ports(name: str) -> dict[str, dict[str, Any]]:
    data = cast(dict[str, Any], json.loads(composed(name)))
    return {
        p["name"]: p
        for p in data["peripherals"]
        if p["name"].startswith("GPIO") and "alias_of" not in p
    }


def test_the_shipped_u5_parts_are_the_45_of_the_u575_and_u585() -> None:
    assert len(U5_PARTS) == 45


@pytest.mark.parametrize("name", U5_PARTS)
def test_a_u5_port_carries_lpgpio1_exactly_on_its_bonded_pads(name: str) -> None:
    ports = _ports(name)
    found: dict[str, int] = {}
    for port in "ABCDEFGHI":
        for entry in ports[f"GPIO{port}"]["alternates"]:
            if entry["signal"].startswith("lpgpio1_p"):
                assert entry["af"] == 11
                found[f"P{port}{entry['pin']}"] = int(entry["signal"].removeprefix("lpgpio1_p"))
    expected = {
        pad: index
        for pad, index in LPGPIO1_PADS.items()
        if ports[f"GPIO{pad[1]}"]["bonded"] >> int(pad[2:]) & 1
    }
    assert found == expected
    assert len(found) >= 7


def test_the_lpgpio1_alternates_are_the_datasheet_pad_table() -> None:
    ports = _ports("STM32U575AGIx")
    found = {
        f"P{port}{entry['pin']}": int(entry["signal"].removeprefix("lpgpio1_p"))
        for port in "ABCDEFGHI"
        for entry in ports[f"GPIO{port}"]["alternates"]
        if entry["signal"].startswith("lpgpio1_p")
    }
    assert found == LPGPIO1_PADS


@pytest.mark.parametrize("name", U5_PARTS)
def test_a_u5_port_keeps_its_alternates_sorted_and_unique(name: str) -> None:
    for port in _ports(name).values():
        keys = [(e["pin"], e["af"], e["signal"]) for e in port["alternates"]]
        assert keys == sorted(set(keys))


@pytest.mark.parametrize("name", [n for n in shipped_names() if n not in U5_PARTS])
def test_no_other_shipped_part_carries_an_lpgpio1_alternate(name: str) -> None:
    for port in _ports(name).values():
        assert not any(e["signal"].startswith("lpgpio1") for e in port["alternates"])
