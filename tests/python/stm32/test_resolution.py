from __future__ import annotations

import json
import os
import shutil
from collections import defaultdict
from fnmatch import fnmatchcase
from pathlib import Path
from typing import Any

import pytest
from popcorn import _core

from popcorn_build import resolution_diff as golden_tool
from popcorn_build.resolution_diff import FAMILIES, GOLDEN_DIR, family_of

FAMILY_KEYS = list(FAMILIES.values())

OVERLAPS = {
    ("flash", "flash/v1.0.0_U5_Cube/STM32U535*,STM32U545*/*"),
    ("flash", "flash/v1.0.0_U5_Cube/STM32U59*,STM32U5A*,STM32U5F*,STM32U5G*/*"),
}


def stored(family: str) -> dict[str, Any]:
    return json.loads((GOLDEN_DIR / f"{family}.json").read_text())


def instances_of(golden: dict[str, Any]) -> dict[str, dict[str, list[Any]]]:
    found: dict[str, dict[str, list[Any]]] = {}
    for group in golden["groups"]:
        for part in group["parts"]:
            found[part] = group["instances"]
    for name, probe in golden["probes"].items():
        found[f"probe:{name}"] = probe["instances"]
    return found


def diff(old: dict[str, list[Any]], new: dict[str, list[Any]]) -> str:
    lines = [
        f"{instance}: {old.get(instance)} -> {new.get(instance)}"
        for instance in sorted(set(old) | set(new))
        if old.get(instance) != new.get(instance)
    ]
    return "\n".join(lines)


@pytest.mark.parametrize("family", FAMILY_KEYS)
def test_the_resolution_golden_is_current(family: str) -> None:
    current = golden_tool.compute(family)
    expected = stored(family)
    assert json.loads(golden_tool.dumps(current)) == expected, "\n".join(
        f"{name}\n{diff(instances_of(expected)[name], instances_of(current).get(name, {}))}"
        for name in instances_of(expected)
        if instances_of(expected)[name] != instances_of(current).get(name)
    )


def walked_resolutions() -> list[Any]:
    from popcorn.stm32 import mcu, shipped_parts

    found = [r for part in shipped_parts() for r in _core.resolve_rows(mcu(part))]
    for probe in golden_tool.probes():
        found.extend(_core.resolve_rows(golden_tool.probe_description(probe)))
    return found


def test_an_instance_matches_one_row_but_the_listed_overlaps() -> None:
    resolutions = walked_resolutions()
    assert {(r.ip, r.row_label) for r in resolutions if len(r.matches) > 1} == OVERLAPS
    assert max(len(r.matches) for r in resolutions) == 2


def test_every_row_serves_a_shipped_part_or_a_probe() -> None:
    labels = {
        entry[0]
        for family in FAMILY_KEYS
        for instances in instances_of(stored(family)).values()
        for entry in instances.values()
        if entry[0]
    }
    dead = [row.label for row in _core.model_rows() if row.label not in labels]
    assert dead == []


def all_part_names() -> list[str]:
    from popcorn.stm32 import shipped_parts

    return [*shipped_parts(), *(p["part"] for p in golden_tool.probes())]


GLOB_EXCEPTIONS = [
    {
        "row": "DBGMCU/-/STM32U5*,STM32H5*,STM32L1*,STM32L4*/*",
        "glob": "STM32H5*",
        "part": "STM32H523CCTx",
        "reason": "the shipped H5 lines call the IP DBG, H523 and H533 call it DBGMCU",
    }
]


def test_every_part_glob_names_one_family_its_row_serves() -> None:
    served: dict[str, set[str]] = defaultdict(set)
    for family in FAMILY_KEYS:
        for label, families in stored(family)["rows"].items():
            served[label].update(families)
    names = all_part_names()
    offenders: list[str] = []
    for row in _core.model_rows():
        for glob in row.part_globs:
            family = FAMILIES.get(glob[:7])
            if family is None:
                offenders.append(f"{row.label}: {glob} starts with no family prefix")
                continue
            wider = sorted(n for n in names if fnmatchcase(n, glob) and family_of(n) != family)
            if wider:
                offenders.append(f"{row.label}: {glob} also matches {wider}")
            listed = any(e["row"] == row.label and e["glob"] == glob for e in GLOB_EXCEPTIONS)
            if family not in served[row.label] and not listed:
                offenders.append(f"{row.label}: {glob}, the row serves {sorted(served[row.label])}")
    assert offenders == []


def test_diff_exits_non_zero_when_a_column_outside_the_allowed_ones_moves(tmp_path: Path) -> None:
    changed = tmp_path / "changed"
    shutil.copytree(GOLDEN_DIR, changed)
    path = changed / "u5.json"
    path.write_text(path.read_text().replace('", "row", "', '", "none_no_row", "', 1))
    args = ["diff", str(GOLDEN_DIR), str(changed)]
    assert golden_tool.main(args) == 1
    assert golden_tool.main([*args, "--allow", "label"]) == 1
    assert golden_tool.main([*args, "--allow", "source"]) == 0


def test_row_labels_are_unique() -> None:
    labels = [row.label for row in _core.model_rows()]
    assert len(labels) == len(set(labels))


@pytest.mark.skipif(
    not os.environ.get("POPCORN_STM32_PIN_DATA"), reason="POPCORN_STM32_PIN_DATA is not set"
)
@pytest.mark.parametrize("exception", GLOB_EXCEPTIONS, ids=lambda e: e["part"])
def test_a_listed_glob_exception_is_served_by_its_row_for_the_part(
    exception: dict[str, str],
) -> None:
    from popcorn.stm32 import mcu

    labels = {r.row_label for r in _core.resolve_rows(mcu(exception["part"]))}
    assert exception["row"] in labels, exception["reason"]


def test_every_row_is_built_by_name() -> None:
    unnamed = {r.row_label for r in walked_resolutions() if r.row_label and r.factory in ("", "?")}
    assert unnamed == set()


def versioned_patches() -> list[tuple[Path, dict[str, str]]]:
    from popcorn.stm32.description import _sources
    from popcorn.stm32.description._patches import read_patch

    found: list[tuple[Path, dict[str, str]]] = []
    for path in sorted((Path(_sources.__file__).parent / "patches").glob("*.toml")):
        patch = read_patch(path)
        if patch.ip is not None and patch.ip.versions:
            found.append((path, patch.ip.versions))
    return found


@pytest.mark.parametrize("probe", golden_tool.probes(), ids=lambda p: p["name"])
def test_a_probe_carries_the_versions_of_the_patches_that_reach_its_part(
    probe: dict[str, Any],
) -> None:
    from popcorn.stm32.description._patches import load_patches

    svd = f"{probe['part'][:7]}xx.svd"
    description = golden_tool.probe_description(probe)
    checked = 0
    for path, versions in versioned_patches():
        for glob, version in versions.items():
            matched = [
                p
                for p in description.peripherals
                if p.alias_of is None and fnmatchcase(p.name, glob)
            ]
            if not matched:
                continue
            reached = bool(load_patches([path], part=probe["part"], svd_name=svd))
            carried = all(p.ip_version == version for p in matched)
            assert reached == carried, f"{probe['name']}: {path.name} {glob}"
            checked += 1
    assert checked > 0
