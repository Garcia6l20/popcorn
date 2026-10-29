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
from popcorn.stm32.description._errors import MissingSource
from popcorn.stm32.description._model import RegionDesc
from popcorn.stm32.description._sources import (
    Sources,
    default_search_paths,
    facts_path,
    find_sources,
)

FACTS_DIR = Path(_cubemx.__file__).parent / "rzone"
FACTS = sorted(FACTS_DIR.glob("*.toml"))

EXPECTED_DIFFERENCES: dict[str, dict[str, tuple[object, object]]] = {
    "STM32_DIE482_786_1024": {"num_interrupts": (125, 126)},
    "STM32_DIE482_786_2048": {"num_interrupts": (125, 126)},
}
"""Per facts file, each value allowed to differ, as (local rzone, facts). RM0456 table 186 gives
the U575 and U585 126 vector positions where the rzone counts 125. Table 186 note 2: position 125 is
reserved on STM32U575/585 rev. X and present on every other revision, so DS13737 3.20.1 counts
125."""


EXPECTED_REGION_DIFFERENCES: dict[str, dict[str, dict[str, tuple[object, object]]]] = {
    stem: {
        "HIGH_CYCLE_FLASH_NS": {"execute": (True, False)},
        "HIGH_CYCLE_FLASH_S": {"execute": (True, False)},
    }
    for stem in ("STM32_DIE484_640_1024", "STM32_DIE484_640_2048")
}
"""Per facts file and region name, each access flag allowed to differ, as (local rzone, facts).
RM0481 7.3.10 lists an instruction fetch from the flash high-cycle data area as a bus error,
where the rzone grants execute."""


def _region_differences(local: RegionDesc, written: RegionDesc) -> dict[str, tuple[object, object]]:
    return {
        flag: (getattr(local.acc, flag), getattr(written.acc, flag))
        for flag in ("read", "write", "execute")
        if getattr(local.acc, flag) != getattr(written.acc, flag)
    }


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
    assert set(EXPECTED_REGION_DIFFERENCES) <= {facts.stem for facts in FACTS}


@pytest.mark.parametrize("stem", ["STM32_DIE484_640_1024", "STM32_DIE484_640_2048"])
@pytest.mark.parametrize("name", ["HIGH_CYCLE_FLASH_NS", "HIGH_CYCLE_FLASH_S"])
def test_the_h5_high_cycle_flash_refuses_instruction_fetch(stem: str, name: str) -> None:
    facts = FACTS_DIR / f"{stem}.toml"
    regions = {r.name: r for r in read_rzone_facts(facts.read_bytes(), file=str(facts)).regions}
    assert (regions[name].acc.read, regions[name].acc.execute) == (True, False)


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
    expected = EXPECTED_REGION_DIFFERENCES.get(facts.stem, {})
    for local_region, written_region in zip(local.regions, written.regions, strict=True):
        assert _region_differences(local_region, written_region) == expected.get(
            local_region.name, {}
        ), local_region.name
        assert replace(written_region, acc=local_region.acc) == local_region, local_region.name


FACTS_PARTS: dict[str, str] = {
    "STM32U575QGIx": "STM32_DIE482_786_1024",
    "STM32U575AIIx": "STM32_DIE482_786_2048",
    "STM32H562RGTx": "STM32_DIE484_640_1024",
    "STM32H573AIIx": "STM32_DIE484_640_2048",
}
"""One part per facts file, built from its facts and from its local rzone."""


def test_every_facts_file_has_a_part_built_from_it() -> None:
    assert sorted(FACTS_PARTS.values()) == [facts.stem for facts in FACTS]


def _assert_facts_build_has_the_local_content(sources: Sources, stem: str) -> None:
    part = read_part(sources.mcu_db.read_bytes(), file=str(sources.mcu_db))
    facts = facts_path(part)
    assert facts.is_file()
    assert facts.stem == stem
    shipped = build_description(replace(sources, rzone=None, rzone_facts=facts))
    local = build_description(sources)
    assert "rzone" not in shipped.sources
    assert shipped.sources["rzone_facts"]

    expected = EXPECTED_REGION_DIFFERENCES.get(facts.stem, {})

    def content(text: str, *, as_written: bool) -> dict[str, object]:
        data = cast(dict[str, object], json.loads(text))
        del data["generator"], data["sources"]
        if as_written:
            for region in cast(list[dict[str, dict[str, bool]]], data["regions"]):
                for flag, (_, written) in expected.get(cast(str, region["name"]), {}).items():
                    region["acc"][flag] = cast(bool, written)
        return data

    assert content(dump_json(shipped), as_written=False) == content(
        dump_json(local), as_written=True
    )


@pytest.mark.timeout(60)
@pytest.mark.parametrize("part", FACTS_PARTS)
def test_a_part_built_from_its_facts_has_the_content_of_the_local_build(part: str) -> None:
    try:
        sources = find_sources(part)
    except MissingSource as exc:
        pytest.skip(f"{part} sources not found: {exc}")
    _assert_facts_build_has_the_local_content(sources, FACTS_PARTS[part])


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
