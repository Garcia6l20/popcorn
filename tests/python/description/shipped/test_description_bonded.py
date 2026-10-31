"""Every GPIO port of a shipped or pin data description carries its bonded pin mask."""

import json
from typing import Any, cast

import pytest
from popcorn.stm32.description._cubemx import bonded_adc_channels, bonded_adc_channels_b, read_part
from popcorn.stm32.description._shipped import build_text, compose_shipped, shipped_names

from ..conftest import require_pin_data

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
    root = require_pin_data()
    ports = _ports(build_text(name, pin_data=root))
    assert ports
    assert [port for port, mask in ports.items() if mask is None] == []
    assert any(mask != 0xFFFF for mask in ports.values())


def test_the_shipped_h573aiix_bonds_ph15_and_pi11() -> None:
    ports = _ports(compose_shipped("STM32H573AIIx"))
    assert ports["GPIOH"] is not None and ports["GPIOH"] >> 15 & 1
    assert ports["GPIOI"] is not None and ports["GPIOI"] >> 11 & 1


def _adcs(text: str) -> dict[str, int | None]:
    data = cast(dict[str, Any], json.loads(text))
    return {
        p["name"]: p.get("bonded_channels")
        for p in data["peripherals"]
        if p["ip"] == "ADC" and "alias_of" not in p
    }


@pytest.mark.parametrize("name", shipped_names())
def test_every_adc_of_a_shipped_part_carries_bonded_channels(name: str) -> None:
    adcs = _adcs(compose_shipped(name))
    assert adcs
    assert [adc for adc, mask in adcs.items() if mask is None] == []


@pytest.mark.parametrize("name", PIN_DATA_PARTS)
def test_every_adc_of_a_pin_data_build_carries_bonded_channels(name: str) -> None:
    root = require_pin_data()
    adcs = _adcs(build_text(name, pin_data=root))
    assert adcs
    assert [adc for adc, mask in adcs.items() if mask is None] == []


def test_the_shipped_l152_parts_bond_their_bank_a_adc_in_pads() -> None:
    assert _adcs(compose_shipped("STM32L152RETx")) == {"ADC": 0x003CFFFF}
    assert _adcs(compose_shipped("STM32L152RBTx")) == {"ADC": 0x003CFFFF}


def test_the_shipped_parts_bond_every_external_channel_of_adc1_and_adc2() -> None:
    assert _adcs(compose_shipped("STM32U575QGIx"))["ADC1"] == 0x3FFFE
    assert _adcs(compose_shipped("STM32H573AIIx")) == {"ADC1": 0xCFFFF, "ADC2": 0xCFFFF}


def _adcs_b(text: str) -> dict[str, int | None]:
    data = cast(dict[str, Any], json.loads(text))
    return {
        p["name"]: p.get("bonded_channels_b")
        for p in data["peripherals"]
        if p["ip"] == "ADC" and "alias_of" not in p
    }


def test_the_shipped_l152_parts_bond_bank_b_only_with_adc_cfg() -> None:
    assert _adcs_b(compose_shipped("STM32L152RETx")) == {"ADC": 0x0001}
    assert _adcs_b(compose_shipped("STM32L152RCTx")) == {"ADC": 0}
    assert _adcs_b(compose_shipped("STM32L152RBTx")) == {"ADC": None}
    assert set(_adcs_b(compose_shipped("STM32U575QGIx")).values()) == {None}
    assert set(_adcs_b(compose_shipped("STM32H573AIIx")).values()) == {None}


@pytest.mark.parametrize(
    ("name", "bank_a", "bank_b"),
    [
        ("STM32L152ZETx", 0xFBFCFFFF, 0x1FCF),
        ("STM32L152RETx", 0x003CFFFF, 0x0001),
        ("STM32L152RCTx", 0x003CFFFF, 0),
        ("STM32L152RBTx", 0x003CFFFF, None),
    ],
)
def test_bonded_adc_channels_read_bank_a_and_the_adc_in_b_signals_apart(
    name: str, bank_a: int, bank_b: int | None
) -> None:
    root = require_pin_data()
    built = build_text(name, pin_data=root)
    assert _adcs(built) == {"ADC": bank_a}
    assert _adcs_b(built) == {"ADC": bank_b}


def test_the_pf13_pad_of_the_144_pin_l152_carries_adc_in3b() -> None:
    root = require_pin_data()
    part = read_part((root / "mcu" / "STM32L152ZETx.xml").read_bytes(), file="STM32L152ZETx.xml")
    assert "ADC_IN3b" in part.pins["PF13"]
    assert bonded_adc_channels_b(part) == {"ADC": 0x1FCF}
    assert bonded_adc_channels(part)["ADC"] & 1 << 3
