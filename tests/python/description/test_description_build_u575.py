import json
from pathlib import Path
from typing import cast

import jsonschema.validators
from popcorn.stm32.description._binder import dump_json, load_json
from popcorn.stm32.description._builder import build_description, resolve_ip
from popcorn.stm32.description._sources import Sources

SCHEMA_PATH = Path(__file__).parents[3] / "data" / "schema" / "mcu_description.schema.json"


def _schema() -> dict[str, object]:
    data = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    assert isinstance(data, dict)
    return cast(dict[str, object], data)


def test_build_u575_matches_expected_shape(u575_sources: Sources) -> None:
    description = build_description(u575_sources)

    assert len(description.peripherals) == 197
    assert sum(1 for p in description.peripherals if p.alias_of is not None) == 98
    assert len(description.regions) == 10
    assert len(description.irqs) == 121

    assert description.sources["svd"]
    assert description.sources["mcu_db"]
    assert description.sources["rzone"]
    assert description.sources["cmsis_header"]
    assert description.sources["patch:stm32u5.toml"]
    assert description.sources["patch:stm32u575.toml"]


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
