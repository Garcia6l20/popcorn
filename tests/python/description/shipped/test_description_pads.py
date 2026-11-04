"""Every ADC of a shipped or pin data description carries its die's channel pad table."""

import json
from pathlib import Path
from typing import Any, cast

import pytest
from popcorn.stm32.description._builder import apply_adc_pads
from popcorn.stm32.description._cubemx import read_part
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._model import (
    Access,
    AdcPadDesc,
    FieldDesc,
    PadRole,
    PeripheralDesc,
    ReadEffect,
    RegisterDesc,
    WriteEffect,
)
from popcorn.stm32.description._shipped import build_text, compose_shipped, shipped_names
from popcorn.stm32.description._sources import pad_reference, pin_data_sources

from ..conftest import require_pin_data

Pads = dict[tuple[int, str], str]


def _peripherals(text: str) -> dict[str, dict[str, Any]]:
    data = cast(dict[str, Any], json.loads(text))
    return {p["name"]: p for p in data["peripherals"] if "alias_of" not in p}


def _pads(peripheral: dict[str, Any]) -> Pads:
    return {
        (pad["channel"], pad["role"]): f"P{pad['port']}{pad['pin']}"
        for pad in peripheral["channel_pins"]
    }


def _shipped_pads(part: str, adc: str) -> Pads:
    return _pads(_peripherals(compose_shipped(part))[adc])


@pytest.mark.parametrize("name", shipped_names())
def test_every_adc_of_a_shipped_part_has_a_pad_for_each_bonded_channel(name: str) -> None:
    adcs = [p for p in _peripherals(compose_shipped(name)).values() if p["ip"] == "ADC"]
    assert adcs
    for adc in adcs:
        pads = _pads(adc)
        assert len(pads) == len(adc["channel_pins"])
        assert adc["channel_pins"] == sorted(
            adc["channel_pins"],
            key=lambda p: (p["channel"], list(PadRole).index(PadRole(p["role"]))),
        )
        bonded = [c for c in range(32) if adc["bonded_channels"] >> c & 1]
        assert [c for c in bonded if (c, "positive") not in pads] == []
        bonded_b = [c for c in range(32) if adc.get("bonded_channels_b", 0) >> c & 1]
        assert [c for c in bonded_b if (c, "bank_b") not in pads] == []


def test_the_l476rc_adc1_in5_is_pa0_and_adc1_and_adc2_in9_share_pa4() -> None:
    adc1 = _shipped_pads("STM32L476RCTx", "ADC1")
    assert adc1[(5, "positive")] == "PA0"
    assert adc1[(9, "positive")] == "PA4"
    assert _shipped_pads("STM32L476RCTx", "ADC2")[(9, "positive")] == "PA4"


def test_the_l476rc_adc3_keeps_its_gpiof_pads_the_lqfp64_does_not_bond() -> None:
    peripherals = _peripherals(compose_shipped("STM32L476RCTx"))
    assert peripherals["GPIOF"]["bonded"] == 0
    assert peripherals["ADC3"]["bonded_channels"] >> 6 & 1 == 0
    pads = _pads(peripherals["ADC3"])
    assert [pads[(c, "positive")] for c in range(6, 14)] == [f"PF{pin}" for pin in range(3, 11)]


def test_the_l152re_bank_b_in0b_is_pb2_and_in27_pf6() -> None:
    pads = _shipped_pads("STM32L152RETx", "ADC")
    assert pads[(0, "bank_b")] == "PB2"
    assert pads[(0, "positive")] == "PA0"
    assert pads[(27, "positive")] == "PF6"


def test_the_cat3_l151vc_in27_comes_from_the_rm0038_table() -> None:
    assert _shipped_pads("STM32L151VCTx", "ADC")[(27, "positive")] == "PF6"


def test_the_l100c6_in22_is_pe7_from_the_l151_reference_of_its_die() -> None:
    peripherals = _peripherals(compose_shipped("STM32L100C6Ux"))
    assert peripherals["ADC"]["bonded_channels"] >> 22 & 1 == 0
    assert _pads(peripherals["ADC"])[(22, "positive")] == "PE7"


def test_the_u575_adc1_in5_is_pa0() -> None:
    assert _shipped_pads("STM32U575ZITxQ", "ADC1")[(5, "positive")] == "PA0"


def test_the_h573_adc1_inp3_is_pa6_and_inn3_pa7_negative() -> None:
    pads = _shipped_pads("STM32H573IIKxQ", "ADC1")
    assert pads[(3, "positive")] == "PA6"
    assert pads[(3, "negative")] == "PA7"


@pytest.mark.timeout(60)
def test_the_l476rc_and_l100c6_pad_references_are_the_first_ranked_parts_of_their_die() -> None:
    root = require_pin_data()
    for part, expected in (
        ("STM32L476RCTx", "STM32L471Z(E-G)Jx"),
        ("STM32L100C6Ux", "STM32L151V(8-B)Hx"),
    ):
        sources = pin_data_sources(part, pin_data=root, core=None)
        assert sources.pad_reference is not None
        assert sources.pad_reference != sources.mcu_db
        reference = read_part(sources.pad_reference.read_bytes(), file=str(sources.pad_reference))
        assert reference.ref_name == expected
        own = read_part(sources.mcu_db.read_bytes(), file=str(sources.mcu_db))
        assert reference.die == own.die
        assert len(reference.pins) > len(own.pins)


@pytest.mark.timeout(60)
def test_a_pin_data_build_of_the_l476rc_keeps_the_adc3_pf3_pad() -> None:
    root = require_pin_data()
    pads = _pads(_peripherals(build_text("STM32L476RCTx", pin_data=root))["ADC3"])
    assert pads[(6, "positive")] == "PF3"


@pytest.mark.timeout(60)
def test_a_pin_data_build_of_the_h573_has_inp3_on_pa6_and_inn3_on_pa7() -> None:
    root = require_pin_data()
    pads = _pads(_peripherals(build_text("STM32H573IIKxQ", pin_data=root))["ADC1"])
    assert pads[(3, "positive")] == "PA6"
    assert pads[(3, "negative")] == "PA7"


def test_the_pad_reference_of_a_part_alone_on_its_die_is_its_own_file(tmp_path: Path) -> None:
    root = require_pin_data()
    sources = pin_data_sources("STM32L476RCTx", pin_data=root, core=None)
    group = read_part(sources.mcu_db.read_bytes(), file=str(sources.mcu_db))
    db = group.select("STM32L476RCTx", file=str(sources.mcu_db))
    alone = tmp_path / sources.mcu_db.name
    alone.write_bytes(sources.mcu_db.read_bytes())
    assert pad_reference(db, alone) == alone


def _adc(ip_version: str, *, bank_b: bool) -> PeripheralDesc:
    fields = (
        [
            FieldDesc(
                name="ADC_CFG",
                lsb=2,
                width=1,
                acc=Access.READ_WRITE,
                on_write=WriteEffect.MODIFY,
                on_read=ReadEffect.NONE,
            )
        ]
        if bank_b
        else []
    )
    return PeripheralDesc(
        name="ADC",
        ip="ADC",
        ip_version=ip_version,
        base=0x40012400,
        size=0x400,
        registers=[
            RegisterDesc(
                name="CR2",
                offset=8,
                width=32,
                reset=0,
                reset_mask=0xFFFFFFFF,
                acc=Access.READ_WRITE,
                fields=fields,
            )
        ],
        cross_rules=[],
        irqs=[],
    )


def _pad(channel: int, role: PadRole, port: str, pin: int) -> AdcPadDesc:
    return AdcPadDesc(channel=channel, role=role, port=port, pin=pin)


def test_the_l1_tables_complete_the_pin_list_pads_in_order() -> None:
    adc = _adc("aditf3_v1_1_Cube", bank_b=True)
    apply_adc_pads("STM32L151VCTx", [adc], {"ADC": [_pad(0, PadRole.POSITIVE, "A", 0)]})
    assert adc.channel_pins is not None
    assert adc.channel_pins[:2] == [
        _pad(0, PadRole.POSITIVE, "A", 0),
        _pad(0, PadRole.BANK_B, "B", 2),
    ]
    assert _pad(27, PadRole.POSITIVE, "F", 6) in adc.channel_pins
    assert len(adc.channel_pins) == 1 + 5 + 11


def test_an_adc_without_adc_cfg_gets_no_bank_b_pads() -> None:
    adc = _adc("aditf3_v1_1_Cube", bank_b=False)
    apply_adc_pads("STM32L151VCTx", [adc], {})
    assert adc.channel_pins is not None
    assert {pad.role for pad in adc.channel_pins} == {PadRole.POSITIVE}


def test_a_pin_list_pad_against_the_rm0038_table_is_refused() -> None:
    adc = _adc("aditf3_v1_1_Cube", bank_b=False)
    with pytest.raises(DescriptionError, match=r"ADC\.channel_pins\.27"):
        apply_adc_pads("STM32L151VCTx", [adc], {"ADC": [_pad(27, PadRole.POSITIVE, "F", 7)]})
