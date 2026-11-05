"""Instances of the H562, H563 and H573 parts against the datasheet peripheral counts."""

import json
import re
from functools import cache
from typing import Any, cast

import pytest
from popcorn.stm32.description._shipped import compose_shipped, shipped_names

PERIPHERALS = ["FDCAN2", "SDMMC2", "DLYBSD2", "FMC", "SPI4", "SPI5", "USART10", "UART8", "ETH"]
H5_PARTS = [n for n in shipped_names() if re.match(r"STM32H5(62|63|73)", n)]


def absent(name: str) -> set[str]:
    """Instances the datasheet table does not give the part, from Table 2 of DS14258 Rev 6 and
    DS14121 Rev 5: the package letter after the line, and the Q suffix of the SMPS variants."""
    line, package, smps = name[6:9], name[9], name.endswith("Q")
    gone: set[str] = set()
    if line == "562":
        gone |= {"FDCAN2", "SDMMC2", "DLYBSD2"}
    if line != "562" and package in "RM":
        gone |= {"SDMMC2", "DLYBSD2"}
    if package == "R":
        gone.add("FMC")
    if package in "RM":
        gone |= {"SPI4", "SPI5", "USART10"}
    if package == "V":
        gone.add("SPI5")
    if package == "M":
        gone.add("UART8")
    if line != "562" and package in "VZ" and smps:
        gone.add("ETH")
    if line == "562":
        gone.add("ETH")
    return gone


@cache
def _names(name: str) -> frozenset[str]:
    description = cast(dict[str, Any], json.loads(compose_shipped(name)))
    return frozenset(p["name"] for p in description["peripherals"])


def test_the_h5_parts_are_listed() -> None:
    assert len(H5_PARTS) == 49


@pytest.mark.parametrize("name", H5_PARTS)
def test_a_part_has_the_instances_its_datasheet_counts(name: str) -> None:
    names = _names(name)
    gone = absent(name)
    for peripheral in PERIPHERALS:
        expected = peripheral not in gone
        assert (peripheral in names) is expected, peripheral
        assert (f"SEC_{peripheral}" in names) is expected, f"SEC_{peripheral}"


def test_every_package_group_is_checked_with_a_part_that_keeps_the_instance() -> None:
    for peripheral in PERIPHERALS:
        kept = [n for n in H5_PARTS if peripheral not in absent(n)]
        dropped = [n for n in H5_PARTS if peripheral in absent(n)]
        assert kept
        assert dropped
