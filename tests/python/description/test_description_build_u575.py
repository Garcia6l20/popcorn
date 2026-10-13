import json
import logging
from dataclasses import replace
from pathlib import Path
from typing import cast

import jsonschema.validators
import pytest
from popcorn.stm32.description._binder import dump_json, load_json
from popcorn.stm32.description._builder import build_description, resolve_ip
from popcorn.stm32.description._model import AlternateDesc
from popcorn.stm32.description._sources import Sources

SCHEMA_PATH = Path(__file__).parents[3] / "data" / "schema" / "part_description.schema.json"


def _schema() -> dict[str, object]:
    data = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    assert isinstance(data, dict)
    return cast(dict[str, object], data)


def test_build_u575_matches_expected_shape(u575_sources: Sources) -> None:
    description = build_description(u575_sources)

    assert len(description.peripherals) == 197
    assert sum(1 for p in description.peripherals if p.alias_of is not None) == 98
    assert len(description.regions) == 14
    assert len(description.irqs) == 121

    assert description.sources["svd"]
    assert description.sources["mcu_db"]
    assert description.sources["rzone"]
    assert description.sources["cmsis_header"]
    assert description.sources["patch:stm32u5.toml"]
    assert description.sources["patch:stm32u575.toml"]
    assert description.sources["gpio_modes"]


def test_build_u575_gpio_ports_carry_their_alternate_functions(u575_sources: Sources) -> None:
    description = build_description(u575_sources)
    by_name = {p.name: p for p in description.peripherals}
    porta = by_name["GPIOA"].alternates
    assert porta is not None
    assert AlternateDesc(pin=6, af=2, signal="tim3_ch1") in porta
    assert AlternateDesc(pin=0, af=1, signal="tim2_ch1") in porta
    assert AlternateDesc(pin=0, af=14, signal="tim2_etr") in porta
    assert porta == sorted(porta, key=lambda a: (a.pin, a.af, a.signal))
    assert by_name["GPIOH"].alternates == []
    assert all(
        p.alternates is None
        for p in description.peripherals
        if not p.name.startswith("GPIO") or p.alias_of is not None
    )


def test_a_modes_file_without_af_numbers_logs_and_carries_none(
    u575_sources: Sources, tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    modes = tmp_path / "GPIO-afio_Modes.xml"
    modes.write_bytes(
        b'<IP xmlns="http://mcd.rou.st.com/modules.php?name=mcu"><GPIO_Pin Name="PA6">'
        b'<PinSignal Name="TIM3_CH1"><SpecificParameter Name="GPIO_AF">'
        b"<PossibleValue>__HAL_AFIO_REMAP_TIM3_DISABLE</PossibleValue>"
        b"</SpecificParameter></PinSignal></GPIO_Pin></IP>"
    )
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        description = build_description(replace(u575_sources, gpio_modes=modes))
    assert all(p.alternates is None for p in description.peripherals)
    assert any("AFIO" in record.message for record in caplog.records)


def test_build_without_a_gpio_modes_file_logs_and_carries_none(
    u575_sources: Sources, caplog: pytest.LogCaptureFixture
) -> None:
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        description = build_description(replace(u575_sources, gpio_modes=None))
    assert all(p.alternates is None for p in description.peripherals)
    assert "gpio_modes" not in description.sources
    assert any("no CubeMX GPIO modes file" in record.message for record in caplog.records)


def test_build_u575_validates_against_schema(u575_sources: Sources) -> None:
    description = build_description(u575_sources)
    text = dump_json(description)
    payload = json.loads(text)

    schema = _schema()
    validator_cls = jsonschema.validators.validator_for(schema)
    validator_cls.check_schema(schema)
    validator_cls(schema).validate(payload)


def test_build_u575_round_trips_through_load_json(u575_sources: Sources) -> None:
    description = build_description(u575_sources)
    text = dump_json(description)
    reloaded = load_json(text, file="in-memory")
    assert reloaded == description


def test_build_u575_adc12_ip_avoids_collision(u575_sources: Sources) -> None:
    description = build_description(u575_sources)
    by_name = {p.name: p for p in description.peripherals}
    assert by_name["ADC12"].ip == "ADC12"


def test_resolve_ip_avoids_synthetic_groupname_collision() -> None:
    real_ip_names = {"FOO"}

    ip, ip_version = resolve_ip("FOO12_COMMON", "FOO", None, None, real_ip_names)
    assert ip == "FOO12_COMMON"
    assert ip_version == ""


def test_resolve_ip_keeps_group_fallback_when_no_collision() -> None:
    real_ip_names = {"BAR"}

    ip, ip_version = resolve_ip("BAZ_COMMON", "BAZGROUP", None, None, real_ip_names)
    assert ip == "BAZGROUP"
    assert ip_version == ""


def test_resolve_ip_prefers_matched_db_ip() -> None:
    ip, ip_version = resolve_ip("FOO1", "FOO", "FOO", "1.0", {"FOO"})
    assert ip == "FOO"
    assert ip_version == "1.0"
