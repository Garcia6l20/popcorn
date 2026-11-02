"""Every GPIO port of a shipped or pin data description carries its bonded pin mask."""

import json
from typing import Any, cast

import pytest
from popcorn.stm32.description._adc_banks import bank_b_pads, bonded_bank_b
from popcorn.stm32.description._builder import apply_bonding
from popcorn.stm32.description._cubemx import (
    bonded_adc_channels,
    bonded_adc_channels_b,
    bonded_pins,
    read_part,
)
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import (
    Access,
    FieldDesc,
    PeripheralDesc,
    ReadEffect,
    RegisterDesc,
    WriteEffect,
)
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


@pytest.mark.timeout(30)
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


@pytest.mark.timeout(30)
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
    assert _adcs_b(compose_shipped("STM32L152RCTx")) == {"ADC": 0x0001}
    assert _adcs_b(compose_shipped("STM32L152RCTxA")) == {"ADC": 0x0001}
    assert _adcs_b(compose_shipped("STM32L152RBTx")) == {"ADC": None}
    assert set(_adcs_b(compose_shipped("STM32U575QGIx")).values()) == {None}
    assert set(_adcs_b(compose_shipped("STM32H573AIIx")).values()) == {None}


@pytest.mark.parametrize(
    ("name", "bank_a", "bank_b"),
    [
        ("STM32L152ZETx", 0xFBFCFFFF, 0x1FCF),
        ("STM32L152RETx", 0x003CFFFF, 0x0001),
        ("STM32L152RCTx", 0x003CFFFF, 0x0001),
        ("STM32L152RBTx", 0x003CFFFF, None),
    ],
)
def test_bank_b_is_bonded_by_the_pads_of_the_package_bank_a_by_the_adc_in_signals(
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


def test_the_bank_b_pads_are_rm0038_table_41() -> None:
    assert dict(bank_b_pads("aditf3_v1_1_Cube")) == {
        0: ("B", 2),
        1: ("F", 11),
        2: ("F", 12),
        3: ("F", 13),
        6: ("F", 14),
        7: ("F", 15),
        8: ("G", 0),
        9: ("G", 1),
        10: ("G", 2),
        11: ("G", 3),
        12: ("G", 4),
    }
    assert bank_b_pads("aditf5_v3_3_Cube") == {}


def test_every_adc_in_b_signal_of_the_l1_pin_data_sits_on_a_bonded_pad_of_the_table() -> None:
    root = require_pin_data()
    files = sorted((root / "mcu").glob("STM32L1*.xml"))
    assert len(files) == 90
    for path in files:
        part = read_part(path.read_bytes(), file=path.name)
        signals = bonded_adc_channels_b(part).get("ADC", 0)
        pads = bonded_bank_b("aditf3_v1_1_Cube", bonded_pins(part))
        assert signals & ~pads == 0, path.name
        assert signals in (0, pads), path.name


def _adc(ip_version: str, *, adc_cfg: bool) -> PeripheralDesc:
    fields = [
        FieldDesc(
            name=name,
            lsb=lsb,
            width=1,
            acc=Access.READ_WRITE,
            on_write=WriteEffect.MODIFY,
            on_read=ReadEffect.NONE,
        )
        for name, lsb in (("ADON", 0), *((("ADC_CFG", 2),) if adc_cfg else ()))
    ]
    cr2 = RegisterDesc(
        name="CR2", offset=8, width=32, reset=0, reset_mask=0xFFFFFFFF, acc=Access.READ_WRITE,
        fields=fields,
    )  # fmt: skip
    return PeripheralDesc(
        name="ADC", ip="ADC", ip_version=ip_version, base=0x40012400, size=0x400,
        registers=[cr2], cross_rules=[], irqs=[],
    )  # fmt: skip


def test_an_adc_cfg_adc_of_a_version_without_a_pad_table_fails_the_build() -> None:
    adc = _adc("aditf9_v1_0_Cube", adc_cfg=True)
    with pytest.raises(DescriptionError, match=r"ADC\.CR2\.ADC_CFG: a bank B pad table"):
        apply_bonding("STM32L1X", [adc], {"B": 1 << 2}, {"ADC": 1})
    assert adc.bonded_channels_b is None


def test_the_bank_b_mask_is_set_only_with_adc_cfg_on_a_version_with_a_pad_table() -> None:
    banked = _adc("aditf3_v1_1_Cube", adc_cfg=True)
    plain = _adc("aditf9_v1_0_Cube", adc_cfg=False)
    apply_bonding("STM32L1X", [banked, plain], {"B": 1 << 2, "F": 1 << 11}, {"ADC": 3})
    assert (banked.bonded_channels, banked.bonded_channels_b) == (3, 0b11)
    assert (plain.bonded_channels, plain.bonded_channels_b) == (3, None)
