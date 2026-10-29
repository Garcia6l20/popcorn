"""Every GPIO port of a shipped or pin data description carries its bonded pin mask."""

import json
import os
from pathlib import Path
from typing import Any, cast

import pytest
from popcorn.stm32.description._shipped import build_text, compose_shipped, shipped_names

PIN_DATA_VARIABLE = "POPCORN_STM32_PIN_DATA"
PIN_DATA_PARTS = ("STM32U575ZGTx", "STM32U575AIIx", "STM32H562RGTx", "STM32H573ZITxQ")


def _ports(text: str) -> dict[str, int | None]:
    data = cast(dict[str, Any], json.loads(text))
    return {
        p["name"]: p.get("bonded")
        for p in data["peripherals"]
        if p["name"].startswith("GPIO") and "alias_of" not in p
    }


@pytest.mark.parametrize("name", shipped_names())
def test_every_gpio_port_of_a_shipped_part_carries_bonded(name: str) -> None:
    ports = _ports(compose_shipped(name))
    assert ports
    assert [port for port, mask in ports.items() if mask is None] == []


@pytest.mark.parametrize("name", PIN_DATA_PARTS)
def test_every_gpio_port_of_a_pin_data_build_carries_bonded(name: str) -> None:
    value = os.environ.get(PIN_DATA_VARIABLE)
    if not value:
        pytest.skip(f"set {PIN_DATA_VARIABLE} to a STM32_open_pin_data checkout")
    ports = _ports(build_text(name, pin_data=Path(value)))
    assert ports
    assert [port for port, mask in ports.items() if mask is None] == []
    assert any(mask != 0xFFFF for mask in ports.values())


def test_the_shipped_h573aiix_bonds_ph15_and_pi11() -> None:
    ports = _ports(compose_shipped("STM32H573AIIx"))
    assert ports["GPIOH"] is not None and ports["GPIOH"] >> 15 & 1
    assert ports["GPIOI"] is not None and ports["GPIOI"] >> 11 & 1
