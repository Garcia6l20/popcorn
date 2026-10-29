"""Layers for a line whose parts differ in instances: `dropped`, the reference rank, the format."""

import json
import lzma
from collections.abc import Callable
from pathlib import Path
from typing import Any, cast

import pytest
from popcorn.stm32.description import _shipped as shipped
from popcorn.stm32.description._cubemx import bonded_pins, expand_refname, read_part
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._layers import (
    LAYER_FORMAT,
    TOP_KEYS,
    choose_reference,
    compose,
    dump,
    factor,
    line_name,
)
from popcorn.stm32.description._shipped import BuiltPart

from ..conftest import require_pin_data

LINE = "STM32T001xx"
INSTANCES = ["E", "B", "D", "A", "C"]
LIMIT = 1 << 20


def _peripheral(name: str) -> dict[str, Any]:
    return {
        "name": name,
        "base": 0x4000_0000 + 0x400 * ord(name),
        "registers": [{"name": f"R{name}", "offset": 0}],
        "cross_rules": [],
    }


def _description(part: str, instances: list[str]) -> dict[str, Any]:
    values: dict[str, Any] = {
        "schema_version": 1,
        "part": part,
        "generator": "hand-written",
        "sources": {},
        "core": {"name": "m3"},
        "regions": [],
        "peripherals": [_peripheral(name) for name in instances],
        "irqs": [],
    }
    return {key: values[key] for key in TOP_KEYS}


def _decode(files: dict[str, bytes], suffix: str) -> dict[str, Any]:
    name = next(name for name in files if name.endswith(suffix))
    return cast(dict[str, Any], json.loads(lzma.decompress(files[name])))


def _compose(files: dict[str, bytes], name: str) -> dict[str, Any]:
    return compose(
        name,
        _decode(files, ".parts.json.xz"),
        _decode(files, ".line.json.xz"),
        _decode(files, ".blocks.json.xz"),
        limit=LIMIT,
    )


def _toy() -> dict[str, dict[str, Any]]:
    return {
        "STM32T001AAAx": _description("STM32T001AAAx", INSTANCES),
        "STM32T001BBBx": _description("STM32T001BBBx", ["E", "D", "C"]),
        "STM32T001CCCx": _description("STM32T001CCCx", ["B", "A"]),
        "STM32T001DDDx": _description("STM32T001DDDx", ["E", "B", "D"]),
    }


def test_parts_dropping_instances_compose_back_byte_equal_in_reference_order() -> None:
    descriptions = _toy()
    files = factor(descriptions, {LINE: "STM32T001AAAx"})
    deltas = _decode(files, ".parts.json.xz")["parts"]
    assert deltas["STM32T001AAAx"]["dropped"] == []
    assert deltas["STM32T001BBBx"]["dropped"] == ["B", "A"]
    assert deltas["STM32T001CCCx"]["dropped"] == ["E", "D", "C"]
    assert deltas["STM32T001DDDx"]["dropped"] == ["A", "C"]
    for name, description in descriptions.items():
        assert dump(_compose(files, name)) == dump(description)
    assert [p["name"] for p in _compose(files, "STM32T001CCCx")["peripherals"]] == ["B", "A"]


def test_the_layers_carry_the_format_the_loader_reads() -> None:
    files = factor(_toy(), {LINE: "STM32T001AAAx"})
    assert LAYER_FORMAT == 2
    assert [
        _decode(files, s)["format"] for s in (".parts.json.xz", ".line.json.xz", ".blocks.json.xz")
    ] == [2, 2, 2]


def test_a_part_with_an_instance_its_reference_lacks_is_refused() -> None:
    descriptions = _toy()
    descriptions["STM32T001BBBx"] = _description("STM32T001BBBx", ["E", "C", "F"])
    with pytest.raises(DescriptionError) as excinfo:
        factor(descriptions, {LINE: "STM32T001AAAx"})
    assert (excinfo.value.file, excinfo.value.key) == ("STM32T001BBBx", "peripherals")


@pytest.mark.parametrize("instances", [["C", "E"], ["E", "E", "C"]], ids=["order", "repeat"])
def test_a_part_listing_kept_instances_out_of_order_or_twice_is_refused(
    instances: list[str],
) -> None:
    descriptions = _toy()
    descriptions["STM32T001BBBx"] = _description("STM32T001BBBx", instances)
    with pytest.raises(DescriptionError):
        factor(descriptions, {LINE: "STM32T001AAAx"})


def _tampered(mutate: Callable[[dict[str, Any]], None]) -> DescriptionError:
    files = factor(_toy(), {LINE: "STM32T001AAAx"})
    parts = _decode(files, ".parts.json.xz")
    mutate(parts["parts"]["STM32T001BBBx"])
    with pytest.raises(DescriptionError) as excinfo:
        compose(
            "STM32T001BBBx",
            parts,
            _decode(files, ".line.json.xz"),
            _decode(files, ".blocks.json.xz"),
            limit=LIMIT,
        )
    return excinfo.value


def test_a_delta_without_dropped_is_refused() -> None:
    error = _tampered(lambda delta: delta.pop("dropped"))
    assert error.key == "parts.STM32T001BBBx.dropped"


@pytest.mark.parametrize("value", ["B", {"B": 1}, None])
def test_a_dropped_that_is_no_list_is_refused(value: object) -> None:
    error = _tampered(lambda delta: delta.update(dropped=value))
    assert error.key == "parts.STM32T001BBBx.dropped"


@pytest.mark.parametrize("dropped", [["B", "B", "A"], ["A", "B"]], ids=["repeat", "swapped"])
def test_a_dropped_that_repeats_or_leaves_the_line_order_is_refused(dropped: list[str]) -> None:
    error = _tampered(lambda delta: delta.update(dropped=dropped))
    assert error.key == "parts.STM32T001BBBx.dropped"


def test_a_repeated_dropped_entry_is_refused_before_the_filter_runs() -> None:
    error = _tampered(lambda delta: delta.update(dropped=["B"] * 1_000_000))
    assert error.key == "parts.STM32T001BBBx.dropped"


@pytest.mark.parametrize("entry", ["NOSUCH", ["B"], 3])
def test_a_dropped_entry_that_is_no_line_instance_is_refused(entry: object) -> None:
    error = _tampered(lambda delta: delta["dropped"].append(entry))
    assert error.key.startswith("parts.STM32T001BBBx.dropped.")


def test_an_override_of_a_dropped_instance_is_refused() -> None:
    def override(delta: dict[str, Any]) -> None:
        delta["peripherals"]["B"] = {"name": "B", "block": "0" * 64}

    error = _tampered(override)
    assert error.key == "parts.STM32T001BBBx.peripherals.B"


def test_a_reference_with_the_most_instances_wins_over_more_bonded_pins() -> None:
    ranks = {"STM32T001AAAx": (4, 100, 512), "STM32T001BBBx": (5, 80, 256)}
    assert choose_reference(ranks) == {LINE: "STM32T001BBBx"}


@pytest.mark.parametrize(
    ("ranks", "reference"),
    [
        ({"STM32T001AAAx": (5, 90, 256), "STM32T001BBBx": (5, 80, 512)}, "STM32T001AAAx"),
        ({"STM32T001AAAx": (5, 90, 256), "STM32T001BBBx": (5, 90, 512)}, "STM32T001BBBx"),
        ({"STM32T001BBBx": (5, 90, 512), "STM32T001AAAx": (5, 90, 512)}, "STM32T001AAAx"),
    ],
    ids=["pins second", "flash third", "name last"],
)
def test_equal_instance_counts_rank_by_pins_flash_and_name(
    ranks: dict[str, tuple[int, int, int]], reference: str
) -> None:
    assert choose_reference(ranks) == {LINE: reference}


def test_the_pin_richest_part_lacking_an_instance_is_not_the_reference() -> None:
    descriptions = {
        "STM32T001AAAx": _description("STM32T001AAAx", ["A", "B", "C", "D"]),
        "STM32T001BBBx": _description("STM32T001BBBx", ["A", "B", "C", "D", "E"]),
        "STM32T001CCCx": _description("STM32T001CCCx", ["A", "C"]),
    }
    pins = {"STM32T001AAAx": 115, "STM32T001BBBx": 100, "STM32T001CCCx": 40}
    ranks = {n: (len(d["peripherals"]), pins[n], 384) for n, d in descriptions.items()}
    references = choose_reference(ranks)
    assert references == {LINE: "STM32T001BBBx"}
    files = factor(descriptions, references)
    for name, description in descriptions.items():
        assert dump(_compose(files, name)) == dump(description)
    with pytest.raises(DescriptionError):
        factor(descriptions, {LINE: "STM32T001AAAx"})


def test_each_l1_line_has_a_reference_holding_the_instances_of_all_its_names() -> None:
    pin_data = require_pin_data()
    ranks: dict[str, tuple[int, int, int]] = {}
    instances: dict[str, set[str]] = {}
    for path in sorted((pin_data / "mcu").glob("STM32L1*.xml")):
        for name, _ in expand_refname(path.stem):
            part = read_part(path.read_bytes(), file=str(path), name=name)
            instances[name] = {ip.instance for ip in part.ips}
            bonded = sum(mask.bit_count() for mask in bonded_pins(part).values())
            ranks[name] = (len(instances[name]), bonded, part.flash_kib[0])
    assert len(ranks) == 132
    references = choose_reference(ranks)
    assert sorted(references) == ["STM32L100xx", "STM32L151xx", "STM32L152xx", "STM32L162xx"]
    for line, reference in references.items():
        names = [name for name in instances if line_name(name) == line]
        assert all(instances[name] <= instances[reference] for name in names), line
        if line != "STM32L100xx":
            assert {"FSMC", "SDIO"} <= instances[reference], line
    assert sum("SDIO" in instance_set for instance_set in instances.values()) == 15
    assert sum("FSMC" in instance_set for instance_set in instances.values()) == 9


def test_build_layers_ranks_the_references_by_the_built_instance_count(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    descriptions = {
        "STM32T001AAAx": _description("STM32T001AAAx", ["A", "B", "C", "D"]),
        "STM32T001BBBx": _description("STM32T001BBBx", ["A", "B", "C", "D", "E"]),
        "STM32T001CCCx": _description("STM32T001CCCx", ["A", "C"]),
    }
    pins = {"STM32T001AAAx": 115, "STM32T001BBBx": 100, "STM32T001CCCx": 40}

    def fake(names: list[str], *, pin_data: Path, jobs: int = 1) -> list[BuiltPart]:
        return [
            BuiltPart(text=json.dumps(descriptions[n]), bonded=pins[n], flash_kib=384)
            for n in names
        ]

    monkeypatch.setattr(shipped, "build_each", fake)
    files = shipped.build_layers(sorted(descriptions), pin_data=Path())
    assert _decode(files, ".line.json.xz")["reference"] == "STM32T001BBBx"
    for name, description in descriptions.items():
        assert dump(_compose(files, name)) == dump(description)
