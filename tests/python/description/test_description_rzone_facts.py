"""Packaged rzone facts files against the CubeMX rzone of the same name, when one is installed."""

import json
import tomllib
from dataclasses import replace
from pathlib import Path
from typing import cast

import pytest
from popcorn.stm32.description import _builder, _cubemx
from popcorn.stm32.description._binder import dump_json
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._cubemx import (
    RzoneProcessor,
    read_part,
    read_rzone,
    read_rzone_facts,
)
from popcorn.stm32.description._sources import Sources, default_search_paths, facts_path

FACTS_DIR = Path(_cubemx.__file__).parent / "rzone"
FACTS = sorted(FACTS_DIR.glob("*.toml"))

EXPECTED_DIFFERENCES: dict[str, dict[str, tuple[object, object]]] = {
    "STM32_DIE482_786_1024": {"num_interrupts": (125, 126)},
}
"""Per facts file, each value allowed to differ, as (local rzone, facts). RM0456 table 186 gives
the U575 126 vector positions where the rzone counts 125. Table 186 note 2: position 125 is
reserved on STM32U575/585 rev. X and present on every other revision, so DS13737 3.20.1 counts
125."""


def _core_values(processor: RzoneProcessor) -> dict[str, object]:
    return {
        "fpu_double": processor.fpu == "DP_FPU",
        "dsp": processor.dsp,
        "trustzone": processor.tz,
        "mpu_regions": max(processor.mpu_regions - processor.sec_mpu_regions, 0),
        "mpu_regions_secure": processor.sec_mpu_regions,
        "sau_regions": processor.sau_regions,
        "num_interrupts": processor.num_interrupts,
    }


def _local_rzone(facts: Path) -> Path:
    local = default_search_paths().cubemx_db / "mcu" / "memory" / f"{facts.stem}.xml"
    if not local.is_file():
        pytest.skip(f"no local CubeMX rzone at {local}")
    return local


def test_every_expected_difference_names_a_facts_file() -> None:
    assert FACTS
    assert set(EXPECTED_DIFFERENCES) <= {facts.stem for facts in FACTS}


@pytest.mark.parametrize("facts", FACTS, ids=[facts.stem for facts in FACTS])
def test_facts_match_the_local_rzone(facts: Path) -> None:
    assert tomllib.loads(facts.read_text(encoding="utf-8"))["rzone"] == f"{facts.stem}.xml"
    local_path = _local_rzone(facts)
    local = read_rzone(local_path.read_bytes(), file=str(local_path))
    written = read_rzone_facts(facts.read_bytes(), file=str(facts))
    local_core = _core_values(local.processor)
    written_core = _core_values(written.processor)
    differences = {
        key: (local_core[key], written_core[key])
        for key in local_core
        if local_core[key] != written_core[key]
    }
    assert differences == EXPECTED_DIFFERENCES.get(facts.stem, {})
    assert [region.name for region in written.regions] == [region.name for region in local.regions]
    for local_region, written_region in zip(local.regions, written.regions, strict=True):
        assert written_region == local_region, local_region.name


@pytest.mark.timeout(20)
def test_the_u575_built_from_its_facts_has_the_content_of_the_local_build(
    u575_sources: Sources,
) -> None:
    part = read_part(u575_sources.mcu_db.read_bytes(), file=str(u575_sources.mcu_db))
    facts = facts_path(part)
    assert facts.is_file()
    shipped = build_description(replace(u575_sources, rzone=None, rzone_facts=facts))
    local = build_description(u575_sources)
    assert "rzone" not in shipped.sources
    assert shipped.sources["rzone_facts"]

    def content(text: str) -> dict[str, object]:
        data = cast(dict[str, object], json.loads(text))
        del data["generator"], data["sources"]
        return data

    assert content(dump_json(shipped)) == content(dump_json(local))


@pytest.mark.timeout(15)
def test_a_local_build_reads_the_rzone_and_never_the_facts(
    u575_sources: Sources, monkeypatch: pytest.MonkeyPatch
) -> None:
    read: list[str] = []
    real_read_rzone = read_rzone

    def spy(data: bytes, *, file: str) -> _cubemx.Rzone:
        read.append(file)
        return real_read_rzone(data, file=file)

    def refuse(data: bytes, *, file: str) -> _cubemx.Rzone:
        raise AssertionError(f"facts read from {file}")

    monkeypatch.setattr(_builder, "read_rzone", spy)
    monkeypatch.setattr(_builder, "read_rzone_facts", refuse)
    assert u575_sources.rzone is not None
    part = read_part(u575_sources.mcu_db.read_bytes(), file=str(u575_sources.mcu_db))
    local = build_description(u575_sources)
    both = build_description(replace(u575_sources, rzone_facts=facts_path(part)))
    assert read == [str(u575_sources.rzone)] * 2
    for description in (local, both):
        assert "rzone" in description.sources
        assert "rzone_facts" not in description.sources
